/* -*- indent-tabs-mode: nil -*- */
#include "log/cert_checker.h"

#include <glog/logging.h>
#include <memory>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

#include "log/cert.h"
#include "log/ct_extensions.h"
#include "util/openssl_util.h"  // for LOG_OPENSSL_ERRORS
#include "util/util.h"

using std::string;
using std::unique_ptr;
using std::vector;
using util::ClearOpenSSLErrors;
using util::Status;

namespace cert_trans {

namespace {

Status GetVerifyError(CertChecker::CertVerifyResult result) {
  switch (result) {
    case CertChecker::INVALID_CERTIFICATE_CHAIN:
    case CertChecker::PRECERT_EXTENSION_IN_CERT_CHAIN:
    case CertChecker::UNSUPPORTED_ALGORITHM_IN_CERT_CHAIN:
      return Status(util::error::INVALID_ARGUMENT,
                    "invalid certificate chain");
    case CertChecker::PRECERT_CHAIN_NOT_WELL_FORMED:
      return Status(util::error::INVALID_ARGUMENT, "prechain not well formed");
    case CertChecker::ROOT_NOT_IN_LOCAL_STORE:
      return Status(util::error::FAILED_PRECONDITION, "unknown root");
    case CertChecker::INTERNAL_ERROR:
      return Status(util::error::INTERNAL, "internal error");
    case CertChecker::OK:
      return Status::OK;
  }
  LOG(FATAL) << "Unknown CertChecker error " << result;
}

}  // namespace

CertChecker::~CertChecker() {
  ClearAllTrustedCertificates();
}

bool CertChecker::LoadTrustedCertificates(const string& cert_file) {
  // A read-only BIO.
  BIO* bio_in = BIO_new(BIO_s_file());
  if (bio_in == NULL) {
    LOG_OPENSSL_ERRORS(ERROR);
    return false;
  }

  if (BIO_read_filename(bio_in, cert_file.c_str()) <= 0) {
    BIO_free(bio_in);
    LOG(ERROR) << "Failed to open file " << cert_file << " for reading";
    LOG_OPENSSL_ERRORS(ERROR);
    return false;
  }

  return LoadTrustedCertificatesFromBIO(bio_in);
}

bool CertChecker::LoadTrustedCertificates(const vector<string>& trusted_certs) {
  string concat_certs;
  for (vector<string>::const_iterator it = trusted_certs.begin();
       it != trusted_certs.end(); ++it) {
    concat_certs.append(*it);
  }
  // A read-only memory BIO.
  BIO* bio_in = BIO_new_mem_buf(
      const_cast<void*>(reinterpret_cast<const void*>(concat_certs.c_str())),
      -1  /* no length, since null-terminated */);
  if (bio_in == NULL) {
    LOG_OPENSSL_ERRORS(ERROR);
    return false;
  }

  return LoadTrustedCertificatesFromBIO(bio_in);
}

bool CertChecker::LoadTrustedCertificatesFromBIO(BIO* bio_in) {
  CHECK(bio_in != NULL);
  std::vector<std::pair<string, Cert*> > certs_to_add;
  bool error = false;
  // certs_to_add may be empty if no new certs were added, so keep track of
  // successfully parsed cert count separately.
  size_t cert_count = 0;

  while (!error) {
    X509* x509 = PEM_read_bio_X509(bio_in, NULL, NULL, NULL);
    if (x509 != NULL) {
      // TODO(ekasper): check that the issuing CA cert is temporally valid
      // and at least warn if it isn't.
      unique_ptr<Cert> cert(new Cert(x509));
      string subject_name;
      CertVerifyResult is_trusted = IsTrusted(*cert, &subject_name);
      if (is_trusted != OK && is_trusted != ROOT_NOT_IN_LOCAL_STORE) {
        error = true;
        break;
      }

      ++cert_count;
      if (is_trusted != OK) {
        certs_to_add.push_back(make_pair(subject_name, cert.release()));
      }
    } else {
      // See if we reached the end of the file.
      unsigned long err = ERR_peek_last_error();
      if (ERR_GET_LIB(err) == ERR_LIB_PEM &&
          ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
        ClearOpenSSLErrors();
        break;
      } else {
        // A real error.
        LOG(ERROR) << "Badly encoded certificate file.";
        LOG_OPENSSL_ERRORS(WARNING);
        error = true;
        break;
      }
    }
  }

  BIO_free(bio_in);

  if (error || !cert_count) {
    while (!certs_to_add.empty()) {
      delete certs_to_add.back().second;
      certs_to_add.pop_back();
    }
    return false;
  }

  size_t new_certs = certs_to_add.size();
  while (!certs_to_add.empty()) {
    trusted_.insert(certs_to_add.back());
    certs_to_add.pop_back();
  }
  LOG(INFO) << "Added " << new_certs << " new certificate(s) to trusted store";
  return true;
}

void CertChecker::ClearAllTrustedCertificates() {
  std::multimap<string, const Cert*>::iterator it = trusted_.begin();
  for (; it != trusted_.end(); ++it)
    delete it->second;
  trusted_.clear();
}

Status CertChecker::CheckCertChain(CertChain* chain) const {
  if (chain == NULL || !chain->IsLoaded())
    return Status(util::error::INVALID_ARGUMENT, "invalid certificate chain");

  // Weed out things that should obviously be precert chains instead.
  Cert::Status status =
      chain->LeafCert()->HasCriticalExtension(cert_trans::NID_ctPoison);
  if (status != Cert::TRUE && status != Cert::FALSE) {
    return Status(util::error::INTERNAL, "internal error");
  }
  if (status == Cert::TRUE)
    return Status(util::error::INVALID_ARGUMENT,
                  "precert extension in certificate chain");

  return CheckIssuerChain(chain);
}

Status CertChecker::CheckIssuerChain(CertChain* chain) const {
  if (chain->RemoveCertsAfterFirstSelfSigned() != Cert::TRUE) {
    LOG(ERROR) << "Failed to trim chain";
    return Status(util::error::INTERNAL, "failed to trim chain");
  }

  // Note that it is OK to allow a root cert that is not CA:true
  // because we will later check that it is trusted.
  Cert::Status status = chain->IsValidCaIssuerChainMaybeLegacyRoot();
  if (status == Cert::FALSE)
    return Status(util::error::INVALID_ARGUMENT, "invalid certificate chain");
  if (status != Cert::TRUE) {
    LOG(ERROR) << "Failed to check issuer chain";
    return Status(util::error::INTERNAL, "failed to check issuer chain");
  }

  status = chain->IsValidSignatureChain();
  if (status == Cert::UNSUPPORTED_ALGORITHM) {
    // UNSUPPORTED_ALGORITHM can happen when a weak algorithm (such as MD2)
    // is intentionally not accepted in which case it's correct to say that
    // the chain is invalid.
    // It can also happen when EVP is not properly initialized, in which case
    // it's more of an INTERNAL_ERROR. However a bust setup would manifest
    // itself in many other ways, including failing tests, so we assume the
    // failure is intentional.
    return Status(util::error::INVALID_ARGUMENT,
                  "unsupported algorithm in certificate chain");
  }
  if (status == Cert::FALSE)
    return Status(util::error::INVALID_ARGUMENT, "invalid certificate chain");

  if (status != Cert::TRUE) {
    LOG(ERROR) << "Failed to check signature chain";
    return Status(util::error::INTERNAL, "failed to check signature chain");
  }
  return GetVerifyError(GetTrustedCa(chain));
}

Status CertChecker::CheckPreCertChain(PreCertChain* chain,
                                      string* issuer_key_hash,
                                      string* tbs_certificate) const {
  if (chain == NULL || !chain->IsLoaded())
    return Status(util::error::INVALID_ARGUMENT, "invalid certificate chain");
  Cert::Status status = chain->IsWellFormed();
  if (status == Cert::FALSE)
    return Status(util::error::INVALID_ARGUMENT, "prechain not well formed");
  if (status != Cert::TRUE) {
    LOG(ERROR) << "Failed to check precert chain format";
    return Status(util::error::INTERNAL, "internal error");
  }
  // Check the issuer and signature chain.
  // We do not, at this point, concern ourselves with whether the CA
  // certificate that issued the precert is a Precertificate Signing
  // Certificate (i.e., has restricted Extended Key Usage) or not,
  // since this does not influence the validity of the chain. The
  // purpose of the EKU is effectively to allow CAs to create an
  // intermediate whose scope can be limited to CT precerts only (by
  // making this extension critical).
  // TODO(ekasper): determine (i.e., ask CAs) if CA:false
  // Precertificate Signing Certificates should be tolerated if they
  // have the necessary EKU set.
  // Preference is "no".

  // TODO(pphaneuf): Once Cert::IsWellFormed returns a util::Status,
  // remove the braces and re-use the one above.
  {
    Status status(CheckIssuerChain(chain));
    if (!status.ok())
      return status;
  }

  Cert::Status uses_pre_issuer = chain->UsesPrecertSigningCertificate();
  if (uses_pre_issuer != Cert::TRUE && uses_pre_issuer != Cert::FALSE)
    return Status(util::error::INTERNAL, "internal error");

  string key_hash;
  if (uses_pre_issuer == Cert::TRUE) {
    if (chain->Length() < 3 ||
        chain->CertAt(2)->SPKISha256Digest(&key_hash) != Cert::TRUE)
      return Status(util::error::INTERNAL, "internal error");
  } else if (chain->Length() < 2 ||
             chain->CertAt(1)->SPKISha256Digest(&key_hash) != Cert::TRUE) {
    return Status(util::error::INTERNAL, "internal error");
  }
  // A well-formed chain always has a precert.
  TbsCertificate tbs(*chain->PreCert());
  if (!tbs.IsLoaded() ||
      tbs.DeleteExtension(cert_trans::NID_ctPoison) != Cert::TRUE)
    return Status(util::error::INTERNAL, "internal error");

  // If the issuing cert is the special Precert Signing Certificate,
  // replace the issuer with the one that will sign the final cert.
  // Should always succeed as we've already verified that the chain
  // is well-formed.
  if (uses_pre_issuer == Cert::TRUE &&
      tbs.CopyIssuerFrom(*chain->PrecertIssuingCert()) != Cert::TRUE)
    return Status(util::error::INTERNAL, "internal error");


  string der_tbs;
  if (tbs.DerEncoding(&der_tbs) != Cert::TRUE)
    return Status(util::error::INTERNAL,
                  "could not DER-encode tbs certificate");

  issuer_key_hash->assign(key_hash);
  tbs_certificate->assign(der_tbs);
  return Status::OK;
}

CertChecker::CertVerifyResult CertChecker::GetTrustedCa(
    CertChain* chain) const {
  const Cert* subject = chain->LastCert();
  if (subject == NULL || !subject->IsLoaded()) {
    LOG(ERROR) << "Chain has no valid certs";
    return INTERNAL_ERROR;
  }

  // Look up issuer from the trusted store.
  if (trusted_.empty()) {
    LOG(WARNING) << "No trusted certificates loaded";
    return ROOT_NOT_IN_LOCAL_STORE;
  }

  string subject_name;
  CertVerifyResult is_trusted = IsTrusted(*subject, &subject_name);
  // Either an error, or OK, meaning the last cert is in our trusted store.
  // Note the trusted cert need not necessarily be self-signed.
  if (is_trusted != ROOT_NOT_IN_LOCAL_STORE)
    return is_trusted;

  string issuer_name;
  Cert::Status status = subject->DerEncodedIssuerName(&issuer_name);
  if (status == Cert::ERROR)
    return INTERNAL_ERROR;
  else if (status != Cert::TRUE)
    return INVALID_CERTIFICATE_CHAIN;

  if (subject_name == issuer_name) {
    // Self-signed: no need to scan again.
    return ROOT_NOT_IN_LOCAL_STORE;
  }

  std::pair<std::multimap<string, const Cert*>::const_iterator,
            std::multimap<string, const Cert*>::const_iterator> issuer_range =
      trusted_.equal_range(issuer_name);

  const Cert* issuer = NULL;
  for (std::multimap<string, const Cert*>::const_iterator it =
           issuer_range.first;
       it != issuer_range.second; ++it) {
    const Cert* issuer_cand = it->second;

    Cert::Status ok = subject->IsSignedBy(*issuer_cand);
    if (ok == Cert::UNSUPPORTED_ALGORITHM) {
      // If the cert's algorithm is unsupported, then there's no point
      // continuing: it's unconditionally invalid.
      return UNSUPPORTED_ALGORITHM_IN_CERT_CHAIN;
    }
    if (ok != Cert::TRUE && ok != Cert::FALSE) {
      LOG(ERROR) << "Failed to check signature for trusted root";
      return INTERNAL_ERROR;
    }
    if (ok == Cert::TRUE) {
      issuer = issuer_cand;
      break;
    }
  }

  if (issuer == NULL)
    return ROOT_NOT_IN_LOCAL_STORE;

  // Clone creates a new Cert but AddCert takes ownership even if Clone
  // failed and the cert can't be added, so we don't have to explicitly
  // check for IsLoaded here.
  if (chain->AddCert(issuer->Clone()) != Cert::TRUE) {
    LOG(ERROR) << "Failed to add trusted root to chain";
    return INTERNAL_ERROR;
  }

  return OK;
}

CertChecker::CertVerifyResult CertChecker::IsTrusted(
    const Cert& cert, string* subject_name) const {
  string cert_name;
  Cert::Status status = cert.DerEncodedSubjectName(&cert_name);
  if (status == Cert::ERROR)
    return INTERNAL_ERROR;
  else if (status != Cert::TRUE)
    return INVALID_CERTIFICATE_CHAIN;

  *subject_name = cert_name;

  std::pair<std::multimap<string, const Cert*>::const_iterator,
            std::multimap<string, const Cert*>::const_iterator> cand_range =
      trusted_.equal_range(cert_name);
  for (std::multimap<string, const Cert*>::const_iterator it =
           cand_range.first;
       it != cand_range.second; ++it) {
    const Cert* cand = it->second;
    if (cert.IsIdenticalTo(*cand)) {
      return OK;
    }
  }
  return ROOT_NOT_IN_LOCAL_STORE;
}

}  // namespace cert_trans
