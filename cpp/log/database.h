/* -*- mode: c++; indent-tabs-mode: nil -*- */
#ifndef DATABASE_H
#define DATABASE_H

#include <functional>
#include <glog/logging.h>
#include <memory>
#include <set>
#include <stdint.h>

#include "base/macros.h"
#include "proto/ct.pb.h"

// The |Logged| class needs to provide this interface:
// class Logged {
//  public:
//   // construct an empty instance
//   LoggedBlob();
//
//   // The key used for storage/retrieval in the database, calculated
//   // from the content.
//   std::string Hash() const;
//
//   // The tree signer assigns a sequence number.
//   void clear_sequence_number();
//   void set_sequence_number(int64_t sequence);
//   bool has_sequence_number() const;
//   int64_t sequence_number() const;
//
//   // If the data has a timestamp associated with it, return it: any
//   // STH including this item will have a later timestamp. Return 0 if
//   // there is no timestamp.
//   uint64_t timestamp() const;
//
//   // Serialization of contents (i.e. excluding sequence number and
//   // hash) for storage/retrieval from the database
//   bool SerializeForDatabase(std::string *dst) const;
//   bool ParseFromDatabase(const std::string &src);
//
//   // Serialization for inclusion in the tree (i.e. this is what
//   // clients would hash over).
//   bool SerializeForLeaf(std::string *dst) const;
//
//   // Debugging.
//   std::string DebugString() const;
//
//   // Fill with random content data for testing (no sequence number).
//   void RandomForTest();
// };
//
// NOTE: This is a database interface for the log server.
// Implementations of this interface MUST provide for the same certificate
// being sequenced multiple times in the tree.
// Although the log server implementation which uses this database interface
// should not allow duplicate entries to be created, this code base will also
// support running in a log mirroring mode, and since the RFC does not forbid
// the same certificate appearing multiple times in a log 3rd party logs may
// exhibit this behavour the mirror must permit it too.


template <class Logged>
class ReadOnlyDatabase {
 public:
  typedef std::function<void(const ct::SignedTreeHead&)> NotifySTHCallback;

  enum LookupResult {
    LOOKUP_OK,
    NOT_FOUND,
  };

  class Iterator {
   public:
    Iterator() = default;
    virtual ~Iterator() = default;

    // If there is an entry available, fill *entry and return true,
    // otherwise return false.
    virtual bool GetNextEntry(Logged* entry) = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(Iterator);
  };

  virtual ~ReadOnlyDatabase() = default;

  // Look up by hash. If the entry exists write the result. If the
  // entry is not logged return NOT_FOUND.
  virtual LookupResult LookupByHash(const std::string& hash,
                                    Logged* result) const = 0;

  // Look up by sequence number.
  virtual LookupResult LookupByIndex(int64_t sequence_number,
                                     Logged* result) const = 0;

  // Return the tree head with the freshest timestamp.
  virtual LookupResult LatestTreeHead(ct::SignedTreeHead* result) const = 0;

  // Scan the entries, starting with the given index.
  virtual std::unique_ptr<Iterator> ScanEntries(int64_t start_index) const = 0;

  // Return the number of entries of contiguous entries (what could be
  // put in a signed tree head). This can be greater than the tree
  // size returned by LatestTreeHead.
  virtual int64_t TreeSize() const = 0;

  // Add/remove a callback to be called when a new tree head is
  // available. The pointer is used as a key, so it should be the same
  // in matching add/remove calls.
  //
  // When adding a callback, if we have a current tree head, it will
  // be called right away with that tree head.
  //
  // As a sanity check, all callbacks must be removed before the
  // database instance is destroyed.
  virtual void AddNotifySTHCallback(const NotifySTHCallback* callback) = 0;
  virtual void RemoveNotifySTHCallback(const NotifySTHCallback* callback) = 0;

  virtual void InitializeNode(const std::string& node_id) = 0;
  virtual LookupResult NodeId(std::string* node_id) = 0;

 protected:
  ReadOnlyDatabase() = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(ReadOnlyDatabase);
};


template <class Logged>
class Database : public ReadOnlyDatabase<Logged> {
 public:
  enum WriteResult {
    OK,
    // Create failed, certificate hash is primary key and must exist.
    MISSING_CERTIFICATE_HASH,
    // Create failed, an entry with this hash already exists.
    DUPLICATE_CERTIFICATE_HASH,
    // Update failed, entry does not exist.
    ENTRY_NOT_FOUND,
    // Another entry has this sequence number already.
    SEQUENCE_NUMBER_ALREADY_IN_USE,
    // Timestamp is primary key, it must exist and be unique,
    DUPLICATE_TREE_HEAD_TIMESTAMP,
    MISSING_TREE_HEAD_TIMESTAMP,
  };

  virtual ~Database() = default;

  // Attempt to create a new entry with the status LOGGED.
  // Fail if an entry with this hash already exists.
  WriteResult CreateSequencedEntry(const Logged& logged) {
    CHECK(logged.has_sequence_number());
    CHECK_GE(logged.sequence_number(), 0);
    return CreateSequencedEntry_(logged);
  }

  // Attempt to write a tree head. Fails only if a tree head with this
  // timestamp already exists (i.e., |timestamp| is primary key). Does
  // not check that the timestamp is newer than previous entries.
  WriteResult WriteTreeHead(const ct::SignedTreeHead& sth) {
    if (!sth.has_timestamp())
      return MISSING_TREE_HEAD_TIMESTAMP;
    return WriteTreeHead_(sth);
  }

 protected:
  Database() = default;

  // See the inline methods with similar names defined above for more
  // documentation.
  virtual WriteResult CreateSequencedEntry_(const Logged& logged) = 0;
  virtual WriteResult WriteTreeHead_(const ct::SignedTreeHead& sth) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(Database);
};


namespace cert_trans {


class DatabaseNotifierHelper {
 public:
  typedef std::function<void(const ct::SignedTreeHead&)> NotifySTHCallback;

  DatabaseNotifierHelper() = default;
  ~DatabaseNotifierHelper();

  void Add(const NotifySTHCallback* callback);
  void Remove(const NotifySTHCallback* callback);
  void Call(const ct::SignedTreeHead& sth) const;

 private:
  typedef std::set<const NotifySTHCallback*> Map;

  Map callbacks_;

  DISALLOW_COPY_AND_ASSIGN(DatabaseNotifierHelper);
};


}  // namespace cert_trans

#endif  // DATABASE_H
