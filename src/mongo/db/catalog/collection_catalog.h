/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <functional>
#include <map>
#include <set>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * This class comprises a UUID to collection catalog, allowing for efficient
 * collection lookup by UUID.
 */
using CollectionUUID = UUID;
class Database;

class CollectionCatalog {
    CollectionCatalog(const CollectionCatalog&) = delete;
    CollectionCatalog& operator=(const CollectionCatalog&) = delete;

    friend class iterator;

public:
    using CollectionInfoFn = std::function<bool(const CollectionPtr& collection)>;

    enum class LifetimeMode {
        // Lifetime of writable Collection is managed by an active write unit of work. The writable
        // collection is installed in the catalog during commit.
        kManagedInWriteUnitOfWork,

        // Unmanaged writable Collection usable outside of write unit of work. Users need to commit
        // the Collection to the catalog.
        kUnmanagedClone,

        // Inplace writable access to the Collection currently installed in the catalog. This is
        // only safe when the server is in a state where there can be no concurrent readers.
        kInplace
    };

    class iterator {
    public:
        using value_type = CollectionPtr;

        iterator(OperationContext* opCtx,
                 StringData dbName,
                 uint64_t genNum,
                 const CollectionCatalog& catalog);
        iterator(OperationContext* opCtx,
                 std::map<std::pair<std::string, CollectionUUID>,
                          std::shared_ptr<Collection>>::const_iterator mapIter,
                 uint64_t genNum,
                 const CollectionCatalog& catalog);
        value_type operator*();
        iterator operator++();
        iterator operator++(int);
        boost::optional<CollectionUUID> uuid();

        Collection* getWritableCollection(OperationContext* opCtx, LifetimeMode mode);

        /*
         * Equality operators == and != do not attempt to reposition the iterators being compared.
         * The behavior for comparing invalid iterators is undefined.
         */
        bool operator==(const iterator& other);
        bool operator!=(const iterator& other);

    private:
        /**
         * Check if _mapIter has been invalidated due to a change in the _orderedCollections map. If
         * it has, restart iteration through a call to lower_bound. If the element that the iterator
         * is currently pointing to has been deleted, the iterator will be repositioned to the
         * element that follows it.
         *
         * Returns true if iterator got repositioned.
         */
        bool _repositionIfNeeded();
        bool _exhausted();

        OperationContext* _opCtx;
        std::string _dbName;
        boost::optional<CollectionUUID> _uuid;
        uint64_t _genNum;
        std::map<std::pair<std::string, CollectionUUID>,
                 std::shared_ptr<Collection>>::const_iterator _mapIter;
        const CollectionCatalog* _catalog;
    };

    struct ProfileSettings {
        int level;
        std::shared_ptr<ProfileFilter> filter;  // nullable

        ProfileSettings(int level, std::shared_ptr<ProfileFilter> filter)
            : level(level), filter(filter) {
            // ProfileSettings represents a state, not a request to change the state.
            // -1 is not a valid profiling level: it is only used in requests, to represent
            // leaving the state unchanged.
            invariant(0 <= level && level <= 2,
                      str::stream() << "Invalid profiling level: " << level);
        }

        ProfileSettings() = default;

        bool operator==(const ProfileSettings& other) {
            return level == other.level && filter == other.filter;
        }
    };

    static CollectionCatalog& get(ServiceContext* svcCtx);
    static CollectionCatalog& get(OperationContext* opCtx);
    CollectionCatalog() = default;

    /**
     * This function is responsible for safely setting the namespace string inside 'coll' to the
     * value of 'toCollection'. The caller need not hold locks on the collection.
     *
     * Must be called within a WriteUnitOfWork. The Collection namespace will be set back to
     * 'fromCollection' if the WriteUnitOfWork aborts.
     */
    void setCollectionNamespace(OperationContext* opCtx,
                                Collection* coll,
                                const NamespaceString& fromCollection,
                                const NamespaceString& toCollection);

    void onCloseDatabase(OperationContext* opCtx, std::string dbName);

    /**
     * Register the collection with `uuid`.
     */
    void registerCollection(CollectionUUID uuid, std::shared_ptr<Collection> collection);

    /**
     * Deregister the collection.
     */
    std::shared_ptr<Collection> deregisterCollection(OperationContext* opCtx, CollectionUUID uuid);

    /**
     * Returns the RecoveryUnit's Change for dropping the collection
     */
    std::unique_ptr<RecoveryUnit::Change> makeFinishDropCollectionChange(
        std::shared_ptr<Collection>, CollectionUUID uuid);

    /**
     * Deregister all the collection objects.
     */
    void deregisterAllCollections();

    /**
     * This function gets the Collection pointer that corresponds to the CollectionUUID.
     * The required locks must be obtained prior to calling this function, or else the found
     * Collection pointer might no longer be valid when the call returns.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    Collection* lookupCollectionByUUIDForMetadataWrite(OperationContext* opCtx,
                                                       LifetimeMode mode,
                                                       CollectionUUID uuid);
    CollectionPtr lookupCollectionByUUID(OperationContext* opCtx, CollectionUUID uuid) const;
    std::shared_ptr<const Collection> lookupCollectionByUUIDForRead(OperationContext* opCtx,
                                                                    CollectionUUID uuid) const;


    void makeCollectionVisible(CollectionUUID uuid);

    /**
     * Returns true if the collection has been registered in the CollectionCatalog but not yet made
     * visible.
     */
    bool isCollectionAwaitingVisibility(CollectionUUID uuid) const;

    /**
     * This function gets the Collection pointer that corresponds to the NamespaceString.
     * The required locks must be obtained prior to calling this function, or else the found
     * Collection pointer may no longer be valid when the call returns.
     *
     * Returns nullptr if the namespace is unknown.
     */
    Collection* lookupCollectionByNamespaceForMetadataWrite(OperationContext* opCtx,
                                                            LifetimeMode mode,
                                                            const NamespaceString& nss);
    CollectionPtr lookupCollectionByNamespace(OperationContext* opCtx,
                                              const NamespaceString& nss) const;
    std::shared_ptr<const Collection> lookupCollectionByNamespaceForRead(
        OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * This function gets the NamespaceString from the collection catalog entry that
     * corresponds to CollectionUUID uuid. If no collection exists with the uuid, return
     * boost::none. See onCloseCatalog/onOpenCatalog for more info.
     */
    boost::optional<NamespaceString> lookupNSSByUUID(OperationContext* opCtx,
                                                     CollectionUUID uuid) const;

    /**
     * Returns the UUID if `nss` exists in CollectionCatalog.
     */
    boost::optional<CollectionUUID> lookupUUIDByNSS(OperationContext* opCtx,
                                                    const NamespaceString& nss) const;

    /**
     * Without acquiring any locks resolves the given NamespaceStringOrUUID to an actual namespace.
     * Throws NamespaceNotFound if the collection UUID cannot be resolved to a name, or if the UUID
     * can be resolved, but the resulting collection is in the wrong database.
     */
    NamespaceString resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                 NamespaceStringOrUUID nsOrUUID);

    /**
     * Returns whether the collection with 'uuid' satisfies the provided 'predicate'. If the
     * collection with 'uuid' is not found, false is returned.
     */
    bool checkIfCollectionSatisfiable(CollectionUUID uuid, CollectionInfoFn predicate) const;

    /**
     * This function gets the UUIDs of all collections from `dbName`.
     *
     * If the caller does not take a strong database lock, some of UUIDs might no longer exist (due
     * to collection drop) after this function returns.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<CollectionUUID> getAllCollectionUUIDsFromDb(StringData dbName) const;

    /**
     * This function gets the ns of all collections from `dbName`. The result is not sorted.
     *
     * Caller must take a strong database lock; otherwise, collections returned could be dropped or
     * renamed.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<NamespaceString> getAllCollectionNamesFromDb(OperationContext* opCtx,
                                                             StringData dbName) const;

    /**
     * This functions gets all the database names. The result is sorted in alphabetical ascending
     * order.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<std::string> getAllDbNames() const;

    /**
     * Sets 'newProfileSettings' as the profiling settings for the database 'dbName'.
     */
    void setDatabaseProfileSettings(StringData dbName, ProfileSettings newProfileSettings);

    /**
     * Fetches the profiling settings for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     */
    ProfileSettings getDatabaseProfileSettings(StringData dbName) const;

    /**
     * Fetches the profiling level for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     *
     * There is no corresponding setDatabaseProfileLevel; use setDatabaseProfileSettings instead.
     * This method only exists as a convenience.
     */
    int getDatabaseProfileLevel(StringData dbName) const {
        return getDatabaseProfileSettings(dbName).level;
    }

    /**
     * Clears the database profile settings entry for 'dbName'.
     */
    void clearDatabaseProfileSettings(StringData dbName);

    /**
     * Puts the catalog in closed state. In this state, the lookupNSSByUUID method will fall back
     * to the pre-close state to resolve queries for currently unknown UUIDs. This allows processes,
     * like authorization and replication, which need to do lookups outside of database locks, to
     * proceed.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onCloseCatalog(OperationContext* opCtx);

    /**
     * Puts the catatlog back in open state, removing the pre-close state. See onCloseCatalog.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onOpenCatalog(OperationContext* opCtx);

    /**
     * The epoch is incremented whenever the catalog is closed and re-opened.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * This allows callers to detect an intervening catalog close. For example, closing the catalog
     * must kill all active queries. This is implemented by checking that the epoch has not changed
     * during query yield recovery.
     */
    uint64_t getEpoch() const;

    iterator begin(OperationContext* opCtx, StringData db) const;
    iterator end(OperationContext* opCtx) const;

    /**
     * Lookup the name of a resource by its ResourceId. If there are multiple namespaces mapped to
     * the same ResourceId entry, we return the boost::none for those namespaces until there is
     * only one namespace in the set. If the ResourceId is not found, boost::none is returned.
     */
    boost::optional<std::string> lookupResourceName(const ResourceId& rid);

    /**
     * Removes an existing ResourceId 'rid' with namespace 'entry' from the map.
     */
    void removeResource(const ResourceId& rid, const std::string& entry);

    /**
     * Inserts a new ResourceId 'rid' into the map with namespace 'entry'.
     */
    void addResource(const ResourceId& rid, const std::string& entry);

    /**
     * Commit unmanaged Collection that was acquired by lookupCollectionBy***ForMetadataWrite and
     * lifetime mode kUnmanagedClone.
     */
    void commitUnmanagedClone(OperationContext* opCtx, Collection* collection);

    /**
     * Discard unmanaged Collection that was acquired by lookupCollectionBy***ForMetadataWrite and
     * lifetime mode kUnmanagedClone.
     */
    void discardUnmanagedClone(OperationContext* opCtx, Collection* collection);

private:
    friend class CollectionCatalog::iterator;

    std::shared_ptr<Collection> _lookupCollectionByUUID(WithLock, CollectionUUID uuid) const;

    /**
     * Helper to commit a cloned Collection into the catalog. It takes a vector of commit handlers
     * that are executed in the same critical section that is used to install the Collection into
     * the catalog.
     */
    void _commitWritableClone(
        std::shared_ptr<Collection> cloned,
        boost::optional<Timestamp> commitTime,
        const std::vector<std::function<void(boost::optional<Timestamp>)>>& commitHandlers);

    const std::vector<CollectionUUID>& _getOrdering_inlock(const StringData& db,
                                                           const stdx::lock_guard<Latch>&);
    mutable mongo::Mutex _catalogLock;

    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    boost::optional<
        mongo::stdx::unordered_map<CollectionUUID, NamespaceString, CollectionUUID::Hash>>
        _shadowCatalog;

    using CollectionCatalogMap =
        stdx::unordered_map<CollectionUUID, std::shared_ptr<Collection>, CollectionUUID::Hash>;
    using OrderedCollectionMap =
        std::map<std::pair<std::string, CollectionUUID>, std::shared_ptr<Collection>>;
    using NamespaceCollectionMap =
        stdx::unordered_map<NamespaceString, std::shared_ptr<Collection>>;
    using DatabaseProfileSettingsMap = StringMap<ProfileSettings>;

    CollectionCatalogMap _catalog;
    OrderedCollectionMap _orderedCollections;  // Ordered by <dbName, collUUID> pair
    NamespaceCollectionMap _collections;

    /**
     * Generation number to track changes to the catalog that could invalidate iterators.
     */
    uint64_t _generationNumber = 0;

    // Incremented whenever the CollectionCatalog gets closed and reopened (onCloseCatalog and
    // onOpenCatalog).
    //
    // Catalog objects are destroyed and recreated when the catalog is closed and re-opened. We
    // increment this counter to track when the catalog is reopened. This permits callers to detect
    // after yielding whether their catalog pointers are still valid. Collection UUIDs are not
    // sufficient, since they remain stable across catalog re-opening.
    //
    // A thread must hold the global exclusive lock to write to this variable, and must hold the
    // global lock in at least MODE_IS to read it.
    uint64_t _epoch = 0;

    // Protects _resourceInformation.
    mutable Mutex _resourceLock = MONGO_MAKE_LATCH("CollectionCatalog::_resourceLock");

    // Mapping from ResourceId to a set of strings that contains collection and database namespaces.
    std::map<ResourceId, std::set<std::string>> _resourceInformation;

    // Protects _databaseProfileSettings.
    mutable Mutex _profileSettingsLock =
        MONGO_MAKE_LATCH("CollectionCatalog::_profileSettingsLock");

    /**
     * Contains non-default database profile settings. New collections, current collections and
     * views must all be able to access the correct profile settings for the database in which they
     * reside. Simple database name to struct ProfileSettings map. Access protected by
     * _profileSettingsLock.
     */
    DatabaseProfileSettingsMap _databaseProfileSettings;
};
}  // namespace mongo
