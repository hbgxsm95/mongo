/**
 * Tests that resource consumption metrics are reported in the profiler.
 *
 *  @tags: [
 *    does_not_support_stepdowns,
 *    requires_fcv_47,
 *    requires_getmore,
 *    requires_non_retryable_writes,
 *    requires_profiling,
 *  ]
 */
(function() {
"use strict";

let res = assert.commandWorked(
    db.adminCommand({getParameter: 1, measureOperationResourceConsumption: 1}));
if (!res.measureOperationResourceConsumption) {
    jsTestLog("Skipping test because the 'measureOperationResourceConsumption' flag is disabled");
    return;
}

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const collName = 'coll';
const debugBuild = db.adminCommand('buildInfo').debug;

testDB.dropDatabase();

testDB.setProfilingLevel(2, 0);

let assertMetricsExist = (profilerEntry) => {
    let metrics = profilerEntry.operationMetrics;
    assert.neq(metrics, undefined);

    assert.gte(metrics.docBytesRead, 0);
    assert.gte(metrics.docUnitsRead, 0);
    assert.gte(metrics.idxEntriesRead, 0);
    assert.gte(metrics.keysSorted, 0);

    assert.gte(metrics.cpuMillis, 0);
    assert.gte(metrics.docBytesWritten, 0);
    assert.gte(metrics.docUnitsWritten, 0);
    assert.gte(metrics.docUnitsReturned, 0);
};

const resetProfileColl = {
    name: 'resetProfileColl',
    command: (db) => {
        db.setProfilingLevel(0);
        assert(db.system.profile.drop());
        db.setProfilingLevel(2, 0);
    },
};
const operations = [
    {
        name: 'create',
        command: (db) => {
            assert.commandWorked(db.createCollection(collName));
        },
        profileFilter: {op: 'command', 'command.create': collName},
        profileAssert: (profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'createIndex',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName},
        profileAssert: (profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            // TODO (SERVER-50865): This does not collect metrics for all documents read. Collect
            // metrics for index builds.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'insert',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 1, a: 0}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'findIxScanAndFetch',
        command: (db) => {
            assert.eq(db[collName].find({_id: 1}).itcount(), 1);

            // Spot check that find is reporting operationMetrics in the slow query logs, as should
            // all operations.
            checkLog.containsJson(db.getMongo(), 51803, {
                'command': (obj) => {
                    return obj.find == collName;
                },
                'operationMetrics': (obj) => {
                    return obj.docBytesRead == 29;
                },
            });
        },
        profileFilter: {op: 'query', 'command.find': collName, 'command.filter': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
    {
        name: 'findCollScan',
        command: (db) => {
            assert.eq(db[collName].find().itcount(), 1);
        },
        profileFilter: {op: 'query', 'command.find': collName, 'command.filter': {}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'aggregate',
        command: (db) => {
            assert.eq(db[collName].aggregate([{$project: {_id: 1}}]).itcount(), 1);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'distinct',
        command: (db) => {
            assert.eq(db[collName].distinct("_id").length, 1);
        },
        profileFilter: {op: 'command', 'command.distinct': collName},
        profileAssert: (profileDoc) => {
            // Does not read from the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
    {
        name: 'findAndModify',
        command: (db) => {
            assert(db[collName].findAndModify({query: {_id: 1}, update: {$set: {a: 1}}}));
        },
        profileFilter: {op: 'command', 'command.findandmodify': collName},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
            }
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
    {
        name: 'update',
        command: (db) => {
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {a: 2}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
            }
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
    {
        name: 'count',
        command: (db) => {
            assert.eq(1, db[collName].count());
        },
        profileFilter: {op: 'command', 'command.count': collName},
        profileAssert: (profileDoc) => {
            // Reads from the fast-count, not the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'explain',
        command: (db) => {
            assert.commandWorked(db[collName].find().explain());
        },
        profileFilter: {op: 'command', 'command.explain.find': collName},
        profileAssert: (profileDoc) => {
            // Should not read from the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    // Clear the profile collection so we can easily identify new operations with similar filters as
    // past operations.
    resetProfileColl,
    {
        name: 'explainWithRead',
        command: (db) => {
            assert.commandWorked(db[collName].find().explain('allPlansExecution'));
        },
        profileFilter: {op: 'command', 'command.explain.find': collName},
        profileAssert: (profileDoc) => {
            // Should read from the collection.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'listIndexes',
        command: (db) => {
            assert.eq(db[collName].getIndexes().length, 2);
        },
        profileFilter: {op: 'command', 'command.listIndexes': collName},
        profileAssert: (profileDoc) => {
            // This reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'dropIndex',
        command: (db) => {
            assert.commandWorked(db[collName].dropIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.dropIndexes': collName},
        profileAssert: (profileDoc) => {
            // This reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    resetProfileColl,
    {
        name: 'getMore',
        command: (db) => {
            db[collName].insert({_id: 2, a: 2});
            let cursor = db[collName].find().batchSize(1);
            cursor.next();
            assert.eq(cursor.objsLeftInBatch(), 0);
            // Trigger a getMore
            cursor.next();
        },
        profileFilter: {op: 'getmore', 'command.collection': collName},
        profileAssert: (profileDoc) => {
            // Debug builds may perform extra reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
            }
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'deleteIxScan',
        command: (db) => {
            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Due to a deficiency in the delete path, we read the same document twice.
            // TODO: SERVER-51420
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 58);
            } else {
                assert.gte(profileDoc.docBytesRead, 58);
            }
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
    {
        name: 'deleteCollScan',
        command: (db) => {
            assert.commandWorked(db[collName].remove({}));
        },
        profileFilter: {op: 'remove', 'command.q': {}},
        profileAssert: (profileDoc) => {
            // Due to a deficiency in the delete path, we read the same document twice.
            // TODO: SERVER-51420
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 58);
            } else {
                assert.gte(profileDoc.docBytesRead, 58);
            }
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'dropCollection',
        command: (db) => {
            assert(db[collName].drop());
        },
        profileFilter: {op: 'command', 'command.drop': collName},
        profileAssert: (profileDoc) => {
            // Reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    resetProfileColl,
    {
        name: 'sample',
        command: (db) => {
            // For $sample to use a random cursor, we must have at least 100 documents and a sample
            // size less than 5%.
            for (let i = 0; i < 150; i++) {
                assert.commandWorked(db[collName].insert({_id: i, a: i}));
            }
            assert.eq(db[collName].aggregate([{$sample: {size: 5}}]).itcount(), 5);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (profileDoc) => {
            // The exact amount of data read is not easily calculable.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    {
        name: 'createIndexUnique',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName},
        profileAssert: (profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            // TODO (SERVER-50865): This does not collect metrics for all documents read. Collect
            // metrics for index builds.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 0);
        }
    },
    resetProfileColl,
    {
        name: 'insertUnique',
        command: (db) => {
            assert.commandWorked(db[collName].insert({a: 200}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
    resetProfileColl,
    {
        name: 'insertDup',
        command: (db) => {
            assert.commandFailedWithCode(db[collName].insert({a: 0}), ErrorCodes.DuplicateKey);
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            // Inserting into a unique index requires reading one key.
            assert.eq(profileDoc.idxEntriesRead, 1);
        }
    },
];

let profileColl = testDB.system.profile;
let testOperation = (operation) => {
    jsTestLog("Testing operation: " + operation.name);
    operation.command(testDB);
    if (!operation.profileFilter) {
        return;
    }

    let cursor = profileColl.find(operation.profileFilter);
    assert(cursor.hasNext(),
           "Could not find operation in profiler with filter: " + tojson(operation.profileFilter));
    let entry = cursor.next();
    assert(!cursor.hasNext(), () => {
        return "Filter for profiler matched more than one entry: filter: " +
            tojson(operation.profileFilter) + ", first entry: " + tojson(entry) +
            ", second entry: " + tojson(cursor.next());
    });

    assertMetricsExist(entry);
    if (operation.profileAssert) {
        operation.profileAssert(entry.operationMetrics);
    }
};

operations.forEach((op) => {
    testOperation(op);
});
})();