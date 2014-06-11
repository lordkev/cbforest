//
//  Database.cc
//  CBForest
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Database.hh"
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>


namespace forestdb {

#pragma mark - FILE:

    class Database::File {
    public:
        static File* forPath(std::string path);
        File();
        std::mutex _transactionMutex;
        std::condition_variable _transactionCond;
        Transaction* _transaction;

        static std::unordered_map<std::string, File*> sFileMap;
        static std::mutex sMutex;
    };

    std::unordered_map<std::string, Database::File*> Database::File::sFileMap;
    std::mutex Database::File::sMutex;

    Database::File* Database::File::forPath(std::string path) {
        std::unique_lock<std::mutex> lock(sMutex);
        File* file = sFileMap[path];
        if (!file) {
            file = new File();
            sFileMap[path] = file;
        }
        return file;
    }

    Database::File::File()
    :_transaction(NULL)
    { }

#pragma mark - DATABASE:

    static void check(fdb_status status) {
        if (status != FDB_RESULT_SUCCESS)
            throw error{status};
    }

    DatabaseGetters::DatabaseGetters()
    :_handle(NULL)
    { }

    Database::Database(std::string path, openFlags flags, const config& cfg)
    :_file(File::forPath(path)), _openFlags(flags), _config(cfg)
    {
        check(::fdb_open(&_handle, path.c_str(), (config*)&cfg));
    }

    Database::~Database() {
        if (_handle)
            fdb_close(_handle);
    }

    DatabaseGetters::info DatabaseGetters::getInfo() const {
        info i;
        check(fdb_get_dbinfo(_handle, &i));
        return i;
    }

    std::string DatabaseGetters::filename() const {
        return std::string(this->getInfo().filename);
    }

    bool Database::isReadOnly() const {
        return (_openFlags & FDB_OPEN_FLAG_RDONLY) != 0;
    }

    
#pragma mark - GET:

    static bool checkGet(fdb_status status) {
        if (status == FDB_RESULT_KEY_NOT_FOUND)
            return false;
        check(status);
        return true;
    }

    Document DatabaseGetters::get(slice key, contentOptions options) const {
        Document doc(key);
        read(doc, options);
        return doc;
    }

    Document DatabaseGetters::get(sequence seq, contentOptions options) const {
        Document doc;
        doc._doc.seqnum = seq;
        if (options & kMetaOnly)
            check(fdb_get_metaonly_byseq(_handle, &doc._doc));
        else
            check(fdb_get_byseq(_handle, doc));
        return doc;
    }

    bool DatabaseGetters::read(Document& doc, contentOptions options) const {
        doc.clearMetaAndBody();
        if (options & kMetaOnly)
            return checkGet(fdb_get_metaonly(_handle, doc));
        else
            return checkGet(fdb_get(_handle, doc));
    }

    Document DatabaseGetters::getByOffset(uint64_t offset) {
        Document doc;
        doc._doc.offset = offset;
        checkGet(fdb_get_byoffset(_handle, doc));
        return doc;
    }


#pragma mark - ENUMERATION:


    static fdb_iterator_opt_t iteratorOptions(const DatabaseGetters::enumerationOptions* options) {
        fdb_iterator_opt_t fdbOptions = 0;
        if (options && (options->contentOptions & DatabaseGetters::kMetaOnly))
            fdbOptions |= FDB_ITR_METAONLY;
        if (!(options && options->includeDeleted))
            fdbOptions |= FDB_ITR_NO_DELETES;
        return fdbOptions;
    }
    
    
    DocEnumerator DatabaseGetters::enumerate(slice startKey, slice endKey,
                                             const enumerationOptions* options) {
        fdb_iterator *iterator;
        check(fdb_iterator_init(_handle, &iterator,
                                startKey.buf, startKey.size,
                                endKey.buf, endKey.size,
                                iteratorOptions(options)));
        return DocEnumerator(iterator, options);
    }

    DocEnumerator DatabaseGetters::enumerate(sequence start, sequence end,
                                             const enumerationOptions* options) {
        fdb_iterator *iterator;
        check(fdb_iterator_sequence_init(_handle, &iterator,
                                         start, end,
                                         iteratorOptions(options)));
        return DocEnumerator(iterator, options);
    }

    DocEnumerator DatabaseGetters::enumerate(std::vector<std::string> docIDs,
                                             const enumerationOptions* options)
    {
        if (docIDs.size() == 0)
            return DocEnumerator();
        std::sort(docIDs.begin(), docIDs.end());
        slice startKey = docIDs[0];
        fdb_iterator *iterator;
        check(fdb_iterator_init(_handle, &iterator,
                                startKey.buf, startKey.size,
                                NULL, 0,
                                iteratorOptions(options)));
        return DocEnumerator(iterator, docIDs, options);
    }


    DocEnumerator::DocEnumerator()
    :_iterator(NULL), _docP(NULL)
    { }

    DocEnumerator::DocEnumerator(fdb_iterator* iterator,
                                 const DatabaseGetters::enumerationOptions* options)
    :_iterator(iterator),
     _options(options ?options->contentOptions : DatabaseGetters::kDefaultContent),
     _docP(NULL)
    {
        next();
    }

    DocEnumerator::DocEnumerator(fdb_iterator* iterator,
                                 std::vector<std::string> docIDs,
                                 const Database::enumerationOptions* options)
    :_iterator(iterator),
     _docIDs(docIDs),
     _curDocID(_docIDs.begin()),
     _options(options ?options->contentOptions : DatabaseGetters::kDefaultContent),
     _docP(NULL)
    {
        next();
    }

    DocEnumerator::~DocEnumerator() {
        fprintf(stderr, "enum: ~DocEnumerator\n");
        close();
    }

    void DocEnumerator::close() {
        fprintf(stderr, "enum: close (free %p, close %p)\n", _docP, _iterator);
        fdb_doc_free(_docP);
        _docP = NULL;
        if (_iterator) {
            fdb_iterator_close(_iterator);
            _iterator = NULL;
        }
    }


    bool DocEnumerator::next() {
        if (!_iterator)
            return false;
        fprintf(stderr, "enum: free %p\n", _docP);
        fdb_doc_free(_docP);
        _docP = NULL;

        fdb_status status;
        if (_docIDs.size() == 0) {
            // Regular iteration:
            status = fdb_iterator_next(_iterator, &_docP);
            fprintf(stderr, "enum: fdb_iterator_next --> %d\n", status);
            if (status == FDB_RESULT_ITERATOR_FAIL) {
                close();
                return false;
            }
        } else {
            // Iterating over a vector of docIDs:
           if (_curDocID == _docIDs.end()) {
                fprintf(stderr, "enum: at end of vector\n");
                close();
                return false;
            }
            slice docID = *_curDocID;
            ++_curDocID;
            status = fdb_iterator_seek(_iterator, docID.buf, docID.size);
            fprintf(stderr, "enum: seek to '%*s' --> %d\n", (int)docID.size, docID.buf, status);
            if (status == FDB_RESULT_SUCCESS) {
                status = fdb_iterator_next(_iterator, &_docP);
                fprintf(stderr, "enum: fdb_iterator_next --> %d\n", status);
                if (status != FDB_RESULT_SUCCESS || slice(_docP->key, _docP->keylen) != docID) {
                    // If the current doc doesn't match the docID, then the docID doesn't exist:
                    fdb_doc_free(_docP);
                    fdb_doc_create(&_docP, docID.buf, docID.size, NULL, 0, NULL, 0);
                    if (status == FDB_RESULT_ITERATOR_FAIL)
                        status = FDB_RESULT_SUCCESS;
                }
            }
        }

        check(status);
        return true;
    }


#pragma mark - DOCUMENTS:

    Document::Document() {
        memset(this, 0, sizeof(*this));
    }

    Document::Document(slice key) {
        memset(this, 0, sizeof(*this));
        setKey(key);
    }

    Document::~Document() {
        key().free();
        meta().free();
        body().free();
    }

    void Document::clearMetaAndBody() {
        setMeta(NULL);
        setBody(NULL);
        _doc.seqnum = 0;
        _doc.offset = 0;
        _doc.deleted = false;
    }

    static inline void _assign(void* &buf, size_t &size, slice s) {
        ::free(buf);
        buf = (void*)s.copy().buf;
        size = s.size;
    }

    void Document::setKey(slice key)   {_assign(_doc.key,  _doc.keylen,  key);}
    void Document::setMeta(slice meta) {_assign(_doc.meta, _doc.metalen, meta);}
    void Document::setBody(slice body) {_assign(_doc.body, _doc.bodylen, body);}



#pragma mark - TRANSACTION:


    fdb_handle* Database::beginTransaction(Transaction* t, sequence &startSequence) {
        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        while (_file->_transaction != NULL)
            _file->_transactionCond.wait(lock);

        startSequence = getInfo().last_seqnum;
        fdb_handle* realHandle = _handle;
#if TRANSACTION_IS_PRIVATE
        // Create a snapshot of the real handle to use as my temporary handle,
        // and return the real handle for the transaction object to use:
        fdb_handle* snapshot = NULL;
        check(fdb_snapshot_open(_handle, &snapshot, startSequence));
        _handle = snapshot;
        _file->_transaction = t;
#endif
        _file->_transaction = t;
        return realHandle;
    }

    void Database::endTransaction(fdb_handle* handle) {
        std::unique_lock<std::mutex> lock(_file->_transactionMutex);

#if TRANSACTION_IS_PRIVATE
        // Close the snapshot and restore my real handle:
        if (handle != _handle) {
            fdb_close(_handle);
            _handle = handle;
        }
#else
        _handle = handle;
#endif
        _file->_transaction = NULL;
        _file->_transactionCond.notify_one();
    }


    Transaction::Transaction(Database* db)
    :_db(*db), _state(0)
    {
        _handle = _db.beginTransaction(this, _startSequence);
    }

    Transaction::~Transaction() {
        fdb_status status = FDB_RESULT_SUCCESS;
        if (_state > 0) {
            status = fdb_commit(_handle, FDB_COMMIT_NORMAL);
            if (status != FDB_RESULT_SUCCESS)
                _state = -1;
        }
        if (_state < 0)
            fdb_rollback(&_handle, _startSequence);
        _db.endTransaction(_handle);
        check(status);
    }

    void Transaction::check(fdb_status status) {
        if (status == FDB_RESULT_SUCCESS) {
            if (_state == 0)
                _state = 1;
        } else {
            _state = -1;
            forestdb::check(status); // throw exception
        }
    }

    void Transaction::deleteDatabase() {
        std::string path = _db.filename();
        check(::fdb_close(_handle));
        _handle = NULL;
        if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
            _state = -1;
            check(::fdb_open(&_handle, path.c_str(), &_db._config));
            throw(errno);
        }
    }

    void Transaction::erase() {
        std::string path = _db.filename();
        deleteDatabase();
        check(::fdb_open(&_handle, path.c_str(), &_db._config));
    }

    void Transaction::rollbackTo(sequence seq) {
        check(fdb_rollback(&_handle, seq));
    }

    void Transaction::commit() {
        check(fdb_commit(_handle, FDB_COMMIT_NORMAL));
    }

    void Transaction::write(Document &doc) {
        check(fdb_set(_handle, doc));
    }

    sequence Transaction::set(slice key, slice meta, slice body) {
        Document doc(key);
        doc.setMeta(meta);
        doc.setBody(body);
        write(doc);
        return doc.sequence();
    }

    sequence Transaction::set(slice key, slice body) {
        Document doc(key);
        doc.setBody(body);
        write(doc);
        return doc.sequence();
    }

    void Transaction::del(forestdb::Document &doc) {
        check(fdb_del(_handle, doc));
    }

    void Transaction::del(forestdb::slice key) {
        Document doc(key);
        del(doc);
    }

    void Transaction::del(sequence seq) {
        Document doc = _db.get(seq);
        del(doc);
    }


}