//
//  c4Database.h
//  CBForest
//
//  C API for database and document access.
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Database_h
#define c4Database_h

#include "c4.h"

#ifdef __cplusplus
extern "C" {
#endif

    //////// DATABASES:


    /** Boolean options specified when opening a database or view. */
    typedef enum {
        kC4DB_Create        = 1,    /**< Create the file if it doesn't exist */
        kC4DB_ReadOnly      = 2,    /**< Open file read-only */
        kC4DB_AutoCompact   = 4,    /**< Enable auto-compaction */
    } C4DatabaseFlags;

    /** Encryption algorithms. */
    enum {
        kC4EncryptionNone = 0,      /**< No encryption (default) */
        kC4EncryptionAES256 = 1     /**< AES with 256-bit key */
    };

    typedef struct {
        int32_t algorithm;
        uint8_t bytes[32];
    } C4EncryptionKey;


    /** Opaque handle to an opened database. */
    typedef struct c4Database C4Database;

    /** Opens a database. */
    C4Database* c4db_open(C4Slice path,
                          C4DatabaseFlags flags,
                          const C4EncryptionKey *encryptionKey,
                          C4Error *outError);

    /** Closes the database and frees the object. */
    bool c4db_close(C4Database* database, C4Error *outError);

    /** Closes the database, deletes the file, and frees the object. */
    bool c4db_delete(C4Database* database, C4Error *outError);

    /** Manually compacts the database. */
    bool c4db_compact(C4Database* database, C4Error *outError);

    /** Changes a database's encryption key (removing encryption if it's NULL.) */
    bool c4db_rekey(C4Database* database,
                    const C4EncryptionKey *newKey,
                    C4Error *outError);

    /** Returns the number of (undeleted) documents in the database. */
    uint64_t c4db_getDocumentCount(C4Database* database);

    /** Returns the latest sequence number allocated to a revision. */
    C4SequenceNumber c4db_getLastSequence(C4Database* database);

    /** Begins a transaction.
        Transactions can nest; only the first call actually creates a ForestDB transaction. */
    bool c4db_beginTransaction(C4Database* database,
                               C4Error *outError);

    /** Commits or aborts a transaction. If there have been multiple calls to beginTransaction, it takes the same number of calls to endTransaction to actually end the transaction; only the last one commits or aborts the ForestDB transaction. */
    bool c4db_endTransaction(C4Database* database,
                             bool commit,
                             C4Error *outError);

    /** Is a transaction active? */
    bool c4db_isInTransaction(C4Database* database);


    //////// RAW DOCUMENTS (i.e. info or _local)


    /** Describes a raw document. */
    typedef struct {
        C4Slice key;
        C4Slice meta;
        C4Slice body;
    } C4RawDocument;

    /** Frees the storage occupied by a raw document. */
    void c4raw_free(C4RawDocument* rawDoc);

    /** Reads a raw document from the database. In Couchbase Lite the store named "info" is used for per-database key/value pairs, and the store "_local" is used for local documents. */
    C4RawDocument* c4raw_get(C4Database* database,
                             C4Slice storeName,
                             C4Slice docID,
                             C4Error *outError);

    /** Writes a raw document to the database, or deletes it if both meta and body are NULL. */
    bool c4raw_put(C4Database* database,
                   C4Slice storeName,
                   C4Slice key,
                   C4Slice meta,
                   C4Slice body,
                   C4Error *outError);

    // Store used for database metadata.
    #define kC4InfoStore ((C4Slice){"info", 4})

    // Store used for local (non-replicated) documents.
    #define kC4LocalDocStore ((C4Slice){"_local", 6})


    //////// DOCUMENTS:


    /** Flags describing a document. */
    typedef enum {
        kDeleted        = 0x01,     /**< The document's current revision is deleted. */
        kConflicted     = 0x02,     /**< The document is in conflict. */
        kHasAttachments = 0x04,     /**< The document's current revision has attachments. */

        kExists         = 0x1000    /**< The document exists (i.e. has revisions.) */
    } C4DocumentFlags; // Note: Superset of VersionedDocument::Flags

    /** Flags that apply to a revision. */
    typedef enum {
        kRevDeleted        = 0x01, /**< Is this revision a deletion/tombstone? */
        kRevLeaf           = 0x02, /**< Is this revision a leaf (no children?) */
        kRevNew            = 0x04, /**< Has this rev been inserted since decoding? */
        kRevHasAttachments = 0x08  /**< Does this rev's body contain attachments? */
    } C4RevisionFlags; // Note: Same as Revision::Flags


    /** Describes a version-controlled document. */
    typedef struct {
        C4DocumentFlags flags;      /**< Document flags */
        C4Slice docID;              /**< Document ID */
        C4Slice revID;              /**< RevID of current revision */
        C4SequenceNumber sequence;  /**< Sequence at which doc was last updated */

        struct {
            C4Slice revID;
            C4RevisionFlags flags;
            C4SequenceNumber sequence;
            C4Slice body;
        } selectedRev;
    } C4Document;

    /** Frees a C4Document. */
    void c4doc_free(C4Document *doc);

    /** Gets a document from the database. If there's no such document, the behavior depends on
        the mustExist flag. If it's true, NULL is returned. If it's false, a valid but empty
        C4Document is returned, that doesn't yet exist in the database (but will be added when
        saved.)
        The current revision is selected (if the document exists.) */
    C4Document* c4doc_get(C4Database *database,
                          C4Slice docID,
                          bool mustExist,
                          C4Error *outError);

    /** Gets a document from the database given its sequence number. */
    C4Document* c4doc_getBySequence(C4Database *database,
                                    C4SequenceNumber,
                                    C4Error *outError);

    /** Returns the document type (as set by setDocType.) This value is ignored by CBForest itself; by convention Couchbase Lite sets it to the value of the current revision's "type" property, and uses it as an optimization when indexing a view. */
    C4SliceResult c4doc_getType(C4Document *doc);

    /** Removes all trace of a document and its revisions from the database. */
    bool c4db_purgeDoc(C4Database *db, C4Slice docID, C4Error *outError);


    //////// REVISIONS:


    /** Selects a specific revision of a document (or no revision, if revID is NULL.) */
    bool c4doc_selectRevision(C4Document* doc,
                              C4Slice revID,
                              bool withBody,
                              C4Error *outError);

    /** Selects the current revision of a document.
        (This is the first revision, in the order they appear in the document.) */
    bool c4doc_selectCurrentRevision(C4Document* doc);

    /** Populates the body field of a doc's selected revision,
        if it was initially loaded without its body. */
    bool c4doc_loadRevisionBody(C4Document* doc,
                                C4Error *outError);

    /** Returns true if the body of the selected revision is available,
        i.e. if c4doc_loadRevisionBody() would succeed. */
    bool c4doc_hasRevisionBody(C4Document* doc);

    /** Selects the parent of the selected revision, if it's known, else returns NULL. */
    bool c4doc_selectParentRevision(C4Document* doc);

    /** Selects the next revision in priority order.
        This can be used to iterate over all revisions, starting from the current revision. */
    bool c4doc_selectNextRevision(C4Document* doc);

    /** Selects the next leaf revision; like selectNextRevision but skips over non-leaves. */
    bool c4doc_selectNextLeafRevision(C4Document* doc,
                                      bool includeDeleted,
                                      bool withBody,
                                      C4Error *outError);


    //////// DOCUMENT ENUMERATORS:


    enum {
        kC4Descending           = 0x01, /**< If true, iteration goes by descending document IDs. */
        kC4InclusiveStart       = 0x02, /**< If false, iteration starts just _after_ startDocID. */
        kC4InclusiveEnd         = 0x04, /**< If false, iteration stops just _before_ endDocID. */
        kC4IncludeDeleted       = 0x08, /**< If true, include deleted documents. */
        kC4IncludeNonConflicted = 0x10, /**< If false, include _only_ documents in conflict. */
        kC4IncludeBodies        = 0x20  /**< If false, document bodies will not be preloaded, just
                                   metadata (docID, revID, sequence, flags.) This is faster if you
                                   don't need to access the revision tree or revision bodies. You
                                   can still access all the data of the document, but it will
                                   trigger loading the document body from the database. */
    };
    typedef uint16_t C4EnumeratorFlags;


    /** Options for enumerating over all documents. */
    typedef struct {
        unsigned          skip;     /**< The number of initial results to skip. */
        C4EnumeratorFlags flags;    /**< Option flags */
    } C4EnumeratorOptions;

    /** Default all-docs enumeration options.
        Includes inclusiveStart, inclusiveEnd, includeBodies, includeNonConflicts.
        Does not include descending, skip, includeDeleted. */
    extern const C4EnumeratorOptions kC4DefaultEnumeratorOptions;
    

    /** Opaque handle to a document enumerator. */
    typedef struct C4DocEnumerator C4DocEnumerator;

    /** Frees a C4DocEnumerator handle. */
    void c4enum_free(C4DocEnumerator *e);

    /** Creates an enumerator ordered by sequence.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param since  The sequence number to start _after_. Pass 0 to start from the beginning.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                           C4SequenceNumber since,
                                           const C4EnumeratorOptions *options,
                                           C4Error *outError);

    /** Creates an enumerator ordered by docID.
        Options have the same meanings as in Couchbase Lite.
        There's no 'limit' option; just stop enumerating when you're done.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param startDocID  The document ID to begin at.
        @param endDocID  The document ID to end at.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                           C4Slice startDocID,
                                           C4Slice endDocID,
                                           const C4EnumeratorOptions *options,
                                           C4Error *outError);

    /** Creates an enumerator on a series of document IDs.
        Options have the same meanings as in Couchbase Lite.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param docIDs  Array of doc IDs to traverse in order.
        @param docIDsCount  Number of doc IDs.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateSomeDocs(C4Database *database,
                                            C4Slice docIDs[],
                                            unsigned docIDsCount,
                                            const C4EnumeratorOptions *options,
                                            C4Error *outError);

    /** Returns the next document from an enumerator, or NULL if there are no more.
        The caller is responsible for freeing the C4Document.
        Don't forget to free the enumerator itself when finished with it. */
    C4Document* c4enum_nextDocument(C4DocEnumerator *e,
                                    C4Error *outError);


    //////// INSERTING REVISIONS:


    /** Adds a new revision to a document, as a child of the currently selected revision
        (or as a root revision if there is no selected revision.)
        On success, the new revision will be selected.
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document.
        @param revID  The ID of the revision being inserted.
        @param body  The (JSON) body of the revision.
        @param deleted  True if this revision is a deletion (tombstone).
        @param hasAttachments  True if this revision contains an _attachments dictionary.
        @param allowConflict  If false, and the parent is not a leaf, a 409 error is returned.
        @param outError  Error information is stored here.
        @return The number of revisions added (0 or 1), or -1 on error. */
    int c4doc_insertRevision(C4Document *doc,
                             C4Slice revID,
                             C4Slice body,
                             bool deleted,
                             bool hasAttachments,
                             bool allowConflict,
                             C4Error *outError);

    /** Adds a revision to a document, plus its ancestors (given in reverse chronological order.)
        On success, the new revision will be selected.
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document.
        @param body  The (JSON) body of the new revision.
        @param deleted  True if the new revision is a deletion (tombstone).
        @param hasAttachments  True if the new revision contains an _attachments dictionary.
        @param history  The ancestors' revision IDs, starting with the new revision's,
                        in reverse order.
        @param historyCount  The number of items in the history array.
        @param outError  Error information is stored here.
        @return The number of revisions added to the document, or -1 on error. */

    int c4doc_insertRevisionWithHistory(C4Document *doc,
                                        C4Slice body,
                                        bool deleted,
                                        bool hasAttachments,
                                        C4Slice history[],
                                        unsigned historyCount,
                                        C4Error *outError);

    /** Removes a branch from a document's history. The revID must correspond to a leaf
        revision; that revision and its ancestors will be removed, except for ancestors that are
        shared with another branch.
        If the document has only one branch (no conflicts), the purge will remove every revision,
        and saving the document will purge it (remove it completely from the database.)
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document.
        @param revID  The ID of the revision to purge.
        @param outError  Error information is stored here.
        @return  The total number of revisions purged (including ancestors), or -1 on error. */
    int c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError);

    /** Sets a document's docType. (By convention this is the value of the "type" property of the current revision's JSON; this value can be used as optimization when indexing a view.)
        The change will not be persisted until the document is saved. */
    bool c4doc_setType(C4Document *doc, C4Slice docType, C4Error *outError);

    /** Saves changes to a C4Document.
        Must be called within a transaction.
        The revision history will be pruned to the maximum depth given. */
    bool c4doc_save(C4Document *doc,
                    unsigned maxRevTreeDepth,
                    C4Error *outError);


#ifdef __cplusplus
}
#endif

#endif /* c4Database_h */
