#pragma once

#include <ext/shared_ptr_helper.h>
#include <atomic>
#include <pcg_random.hpp>
#include <Storages/IStorage.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeDataMergerMutator.h>
#include <Storages/MergeTree/MergeTreePartsMover.h>
#include <Storages/MergeTree/MergeTreeDataWriter.h>
#include <Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <Storages/MergeTree/ReplicatedMergeTreeLogEntry.h>
#include <Storages/MergeTree/ReplicatedMergeTreeQueue.h>
#include <Storages/MergeTree/ReplicatedMergeTreeCleanupThread.h>
#include <Storages/MergeTree/ReplicatedMergeTreeRestartingThread.h>
#include <Storages/MergeTree/ReplicatedMergeTreePartCheckThread.h>
#include <Storages/MergeTree/ReplicatedMergeTreeAlterThread.h>
#include <Storages/MergeTree/ReplicatedMergeTreeTableMetadata.h>
#include <Storages/MergeTree/EphemeralLockInZooKeeper.h>
#include <Storages/MergeTree/BackgroundProcessingPool.h>
#include <Storages/MergeTree/DataPartsExchange.h>
#include <Storages/MergeTree/ReplicatedMergeTreeAddress.h>
#include <DataTypes/DataTypesNumber.h>
#include <Interpreters/InterserverCredentials.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/PartLog.h>
#include <Common/randomSeed.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/LeaderElection.h>
#include <Core/BackgroundSchedulePool.h>
#include <Processors/Pipe.h>


namespace DB
{

/** The engine that uses the merge tree (see MergeTreeData) and replicated through ZooKeeper.
  *
  * ZooKeeper is used for the following things:
  * - the structure of the table (/ metadata, /columns)
  * - action log with data (/log/log-...,/replicas/replica_name/queue/queue-...);
  * - a replica list (/replicas), and replica activity tag (/replicas/replica_name/is_active), replica addresses (/replicas/replica_name/host);
  * - select the leader replica (/leader_election) - this is the replica that assigns the merge;
  * - a set of parts of data on each replica (/replicas/replica_name/parts);
  * - list of the last N blocks of data with checksum, for deduplication (/blocks);
  * - the list of incremental block numbers (/block_numbers) that we are about to insert,
  *   to ensure the linear order of data insertion and data merge only on the intervals in this sequence;
  * - coordinates writes with quorum (/quorum).
  * - Storage of mutation entries (ALTER DELETE, ALTER UPDATE etc.) to execute (/mutations).
  *   See comments in StorageReplicatedMergeTree::mutate() for details.
  */

/** The replicated tables have a common log (/log/log-...).
  * Log - a sequence of entries (LogEntry) about what to do.
  * Each entry is one of:
  * - normal data insertion (GET),
  * - merge (MERGE),
  * - delete the partition (DROP).
  *
  * Each replica copies (queueUpdatingTask, pullLogsToQueue) entries from the log to its queue (/replicas/replica_name/queue/queue-...)
  *  and then executes them (queueTask).
  * Despite the name of the "queue", execution can be reordered, if necessary (shouldExecuteLogEntry, executeLogEntry).
  * In addition, the records in the queue can be generated independently (not from the log), in the following cases:
  * - when creating a new replica, actions are put on GET from other replicas (createReplica);
  * - if the part is corrupt (removePartAndEnqueueFetch) or absent during the check (at start - checkParts, while running - searchForMissingPart),
  *   actions are put on GET from other replicas;
  *
  * The replica to which INSERT was made in the queue will also have an entry of the GET of this data.
  * Such an entry is considered to be executed as soon as the queue handler sees it.
  *
  * The log entry has a creation time. This time is generated by the clock of server that created entry
  * - the one on which the corresponding INSERT or ALTER query came.
  *
  * For the entries in the queue that the replica made for itself,
  * as the time will take the time of creation the appropriate part on any of the replicas.
  */

class StorageReplicatedMergeTree : public ext::shared_ptr_helper<StorageReplicatedMergeTree>, public MergeTreeData
{
    friend struct ext::shared_ptr_helper<StorageReplicatedMergeTree>;
public:
    void startup() override;
    void shutdown() override;
    ~StorageReplicatedMergeTree() override;

    std::string getName() const override { return "Replicated" + merging_params.getModeName() + "MergeTree"; }
    std::string getTableName() const override { return table_name; }
    std::string getDatabaseName() const override { return database_name; }

    bool supportsReplication() const override { return true; }
    bool supportsDeduplication() const override { return true; }

    Pipes readWithProcessors(
        const Names & column_names,
        const SelectQueryInfo & query_info,
        const Context & context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        unsigned num_streams) override;

    bool supportProcessorsPipeline() const override { return true; }

    std::optional<UInt64> totalRows() const override;

    BlockOutputStreamPtr write(const ASTPtr & query, const Context & context) override;

    bool optimize(const ASTPtr & query, const ASTPtr & partition, bool final, bool deduplicate, const Context & query_context) override;

    void alter(const AlterCommands & params, const Context & query_context, TableStructureWriteLockHolder & table_lock_holder) override;

    void alterPartition(const ASTPtr & query, const PartitionCommands & commands, const Context & query_context) override;

    void mutate(const MutationCommands & commands, const Context & context) override;
    std::vector<MergeTreeMutationStatus> getMutationsStatus() const override;
    CancellationCode killMutation(const String & mutation_id) override;

    /** Removes a replica from ZooKeeper. If there are no other replicas, it deletes the entire table from ZooKeeper.
      */
    void drop(TableStructureWriteLockHolder &) override;

    void truncate(const ASTPtr &, const Context &, TableStructureWriteLockHolder &) override;

    void rename(const String & new_path_to_db, const String & new_database_name, const String & new_table_name, TableStructureWriteLockHolder &) override;

    bool supportsIndexForIn() const override { return true; }

    void checkTableCanBeDropped() const override;

    void checkPartitionCanBeDropped(const ASTPtr & partition) override;

    ActionLock getActionLock(StorageActionBlockType action_type) override;

    /// Wait when replication queue size becomes less or equal than queue_size
    /// If timeout is exceeded returns false
    bool waitForShrinkingQueueSize(size_t queue_size = 0, UInt64 max_wait_milliseconds = 0);

    /** For the system table replicas. */
    struct Status
    {
        bool is_leader;
        bool can_become_leader;
        bool is_readonly;
        bool is_session_expired;
        ReplicatedMergeTreeQueue::Status queue;
        UInt32 parts_to_check;
        String zookeeper_path;
        String replica_name;
        String replica_path;
        Int32 columns_version;
        UInt64 log_max_index;
        UInt64 log_pointer;
        UInt64 absolute_delay;
        UInt8 total_replicas;
        UInt8 active_replicas;
    };

    /// Get the status of the table. If with_zk_fields = false - do not fill in the fields that require queries to ZK.
    void getStatus(Status & res, bool with_zk_fields = true);

    using LogEntriesData = std::vector<ReplicatedMergeTreeLogEntryData>;
    void getQueue(LogEntriesData & res, String & replica_name);

    /// Get replica delay relative to current time.
    time_t getAbsoluteDelay() const;

    /// If the absolute delay is greater than min_relative_delay_to_yield_leadership,
    /// will also calculate the difference from the unprocessed time of the best replica.
    /// NOTE: Will communicate to ZooKeeper to calculate relative delay.
    void getReplicaDelays(time_t & out_absolute_delay, time_t & out_relative_delay);

    /// Add a part to the queue of parts whose data you want to check in the background thread.
    void enqueuePartForCheck(const String & part_name, time_t delay_to_check_seconds = 0)
    {
        part_check_thread.enqueuePart(part_name, delay_to_check_seconds);
    }

    CheckResults checkData(const ASTPtr & query, const Context & context) override;

    /// Checks ability to use granularity
    bool canUseAdaptiveGranularity() const override;

private:

    /// Get a sequential consistent view of current parts.
    ReplicatedMergeTreeQuorumAddedParts::PartitionIdToMaxBlock getMaxAddedBlocks() const;

    /// Delete old parts from disk and from ZooKeeper.
    void clearOldPartsAndRemoveFromZK();

    friend class ReplicatedMergeTreeBlockOutputStream;
    friend class ReplicatedMergeTreePartCheckThread;
    friend class ReplicatedMergeTreeCleanupThread;
    friend class ReplicatedMergeTreeAlterThread;
    friend class ReplicatedMergeTreeRestartingThread;
    friend struct ReplicatedMergeTreeLogEntry;
    friend class ScopedPartitionMergeLock;
    friend class ReplicatedMergeTreeQueue;
    friend class MergeTreeData;

    using LogEntry = ReplicatedMergeTreeLogEntry;
    using LogEntryPtr = LogEntry::Ptr;

    zkutil::ZooKeeperPtr current_zookeeper;        /// Use only the methods below.
    mutable std::mutex current_zookeeper_mutex;    /// To recreate the session in the background thread.

    zkutil::ZooKeeperPtr tryGetZooKeeper() const;
    zkutil::ZooKeeperPtr getZooKeeper() const;
    void setZooKeeper(zkutil::ZooKeeperPtr zookeeper);

    /// If true, the table is offline and can not be written to it.
    std::atomic_bool is_readonly {false};

    String zookeeper_path;
    String replica_name;
    String replica_path;

    /** /replicas/me/is_active.
      */
    zkutil::EphemeralNodeHolderPtr replica_is_active_node;

    /** Version of the /columns node in ZooKeeper corresponding to the current data.columns.
      * Read and modify along with the data.columns - under TableStructureLock.
      */
    int columns_version = -1;

    /// Version of the /metadata node in ZooKeeper.
    int metadata_version = -1;

    /// Used to delay setting table structure till startup() in case of an offline ALTER.
    std::function<void()> set_table_structure_at_startup;

    /** Is this replica "leading". The leader replica selects the parts to merge.
      */
    std::atomic<bool> is_leader {false};
    zkutil::LeaderElectionPtr leader_election;

    InterserverIOEndpointHolderPtr data_parts_exchange_endpoint_holder;
    std::shared_ptr<BaseInterserverCredentials> interserver_credentials_;

    MergeTreeDataSelectExecutor reader;
    MergeTreeDataWriter writer;
    MergeTreeDataMergerMutator merger_mutator;

    /** The queue of what needs to be done on this replica to catch up with everyone. It is taken from ZooKeeper (/replicas/me/queue/).
     * In ZK entries in chronological order. Here it is not necessary.
     */
    ReplicatedMergeTreeQueue queue;
    std::atomic<time_t> last_queue_update_start_time{0};
    std::atomic<time_t> last_queue_update_finish_time{0};

    DataPartsExchange::Fetcher fetcher;


    /// When activated, replica is initialized and startup() method could exit
    Poco::Event startup_event;

    /// Do I need to complete background threads (except restarting_thread)?
    std::atomic<bool> partial_shutdown_called {false};

    /// Event that is signalled (and is reset) by the restarting_thread when the ZooKeeper session expires.
    Poco::Event partial_shutdown_event {false};     /// Poco::Event::EVENT_MANUALRESET

    /// Limiting parallel fetches per one table
    std::atomic_uint current_table_fetches {0};

    /// Threads.

    /// A task that keeps track of the updates in the logs of all replicas and loads them into the queue.
    bool queue_update_in_progress = false;
    BackgroundSchedulePool::TaskHolder queue_updating_task;

    BackgroundSchedulePool::TaskHolder mutations_updating_task;

    /// A task that performs actions from the queue.
    BackgroundProcessingPool::TaskHandle queue_task_handle;

    /// A task which move parts to another disks/volumes
    /// Transparent for replication.
    BackgroundProcessingPool::TaskHandle move_parts_task_handle;

    /// A task that selects parts to merge.
    BackgroundSchedulePool::TaskHolder merge_selecting_task;
    /// It is acquired for each iteration of the selection of parts to merge or each OPTIMIZE query.
    std::mutex merge_selecting_mutex;

    /// A task that marks finished mutations as done.
    BackgroundSchedulePool::TaskHolder mutations_finalizing_task;

    /// A thread that removes old parts, log entries, and blocks.
    ReplicatedMergeTreeCleanupThread cleanup_thread;

    /// A thread monitoring changes to the column list in ZooKeeper and updating the parts in accordance with these changes.
    ReplicatedMergeTreeAlterThread alter_thread;

    /// A thread that checks the data of the parts, as well as the queue of the parts to be checked.
    ReplicatedMergeTreePartCheckThread part_check_thread;

    /// A thread that processes reconnection to ZooKeeper when the session expires.
    ReplicatedMergeTreeRestartingThread restarting_thread;

    /// An event that awakens `alter` method from waiting for the completion of the ALTER query.
    zkutil::EventPtr alter_query_event = std::make_shared<Poco::Event>();

    /// True if replica was created for existing table with fixed granularity
    bool other_replicas_fixed_granularity = false;

    /** Creates the minimum set of nodes in ZooKeeper.
      */
    void createTableIfNotExists();

    /** Creates a replica in ZooKeeper and adds to the queue all that it takes to catch up with the rest of the replicas.
      */
    void createReplica();

    /** Create nodes in the ZK, which must always be, but which might not exist when older versions of the server are running.
      */
    void createNewZooKeeperNodes();

    /** Verify that the list of columns and table settings match those specified in ZK (/metadata).
      * If not, throw an exception.
      * Must be called before startup().
      */
    void checkTableStructure(bool skip_sanity_checks, bool allow_alter);

    /// A part of ALTER: apply metadata changes only (data parts are altered separately).
    /// Must be called under IStorage::lockStructureForAlter() lock.
    void setTableStructure(ColumnsDescription new_columns, const ReplicatedMergeTreeTableMetadata::Diff & metadata_diff);

    /** Check that the set of parts corresponds to that in ZK (/replicas/me/parts/).
      * If any parts described in ZK are not locally, throw an exception.
      * If any local parts are not mentioned in ZK, remove them.
      *  But if there are too many, throw an exception just in case - it's probably a configuration error.
      */
    void checkParts(bool skip_sanity_checks);

    /** Check that the part's checksum is the same as the checksum of the same part on some other replica.
      * If no one has such a part, nothing checks.
      * Not very reliable: if two replicas add a part almost at the same time, no checks will occur.
      * Adds actions to `ops` that add data about the part into ZooKeeper.
      * Call under TableStructureLock.
      */
    void checkPartChecksumsAndAddCommitOps(const zkutil::ZooKeeperPtr & zookeeper, const DataPartPtr & part,
                                           Coordination::Requests & ops, String part_name = "", NameSet * absent_replicas_paths = nullptr);

    String getChecksumsForZooKeeper(const MergeTreeDataPartChecksums & checksums) const;

    /// Accepts a PreComitted part, atomically checks its checksums with ones on other replicas and commit the part
    DataPartsVector checkPartChecksumsAndCommit(Transaction & transaction,
                                                               const DataPartPtr & part);

    bool partIsAssignedToBackgroundOperation(const DataPartPtr & part) const override;

    void getCommitPartOps(Coordination::Requests & ops, MutableDataPartPtr & part, const String & block_id_path = "") const;

    /// Updates info about part columns and checksums in ZooKeeper and commits transaction if successful.
    void updatePartHeaderInZooKeeperAndCommit(
        const zkutil::ZooKeeperPtr & zookeeper,
        AlterDataPartTransaction & transaction);

    /// Adds actions to `ops` that remove a part from ZooKeeper.
    /// Set has_children to true for "old-style" parts (those with /columns and /checksums child znodes).
    void removePartFromZooKeeper(const String & part_name, Coordination::Requests & ops, bool has_children);

    /// Quickly removes big set of parts from ZooKeeper (using async multi queries)
    void removePartsFromZooKeeper(zkutil::ZooKeeperPtr & zookeeper, const Strings & part_names,
                                  NameSet * parts_should_be_retried = nullptr);

    bool tryRemovePartsFromZooKeeperWithRetries(const Strings & part_names, size_t max_retries = 5);
    bool tryRemovePartsFromZooKeeperWithRetries(DataPartsVector & parts, size_t max_retries = 5);

    /// Removes a part from ZooKeeper and adds a task to the queue to download it. It is supposed to do this with broken parts.
    void removePartAndEnqueueFetch(const String & part_name);

    /// Running jobs from the queue.

    /** Execute the action from the queue. Throws an exception if something is wrong.
      * Returns whether or not it succeeds. If it did not work, write it to the end of the queue.
      */
    bool executeLogEntry(LogEntry & entry);


    void executeDropRange(const LogEntry & entry);

    /// Do the merge or recommend to make the fetch instead of the merge
    bool tryExecuteMerge(const LogEntry & entry);

    bool tryExecutePartMutation(const LogEntry & entry);


    bool executeFetch(LogEntry & entry);

    void executeClearColumnOrIndexInPartition(const LogEntry & entry);

    bool executeReplaceRange(const LogEntry & entry);

    /** Updates the queue.
      */
    void queueUpdatingTask();

    void mutationsUpdatingTask();

    /** Clone data from another replica.
      * If replica can not be cloned throw Exception.
      */
    void cloneReplica(const String & source_replica, Coordination::Stat source_is_lost_stat, zkutil::ZooKeeperPtr & zookeeper);

    /// Clone replica if it is lost.
    void cloneReplicaIfNeeded(zkutil::ZooKeeperPtr zookeeper);

    /** Performs actions from the queue.
      */
    BackgroundProcessingPoolTaskResult queueTask();

    /// Perform moves of parts to another disks.
    /// Local operation, doesn't interact with replicationg queue.
    BackgroundProcessingPoolTaskResult movePartsTask();


    /// Postcondition:
    /// either leader_election is fully initialized (node in ZK is created and the watching thread is launched)
    /// or an exception is thrown and leader_election is destroyed.
    void enterLeaderElection();

    /// Postcondition:
    /// is_leader is false, merge_selecting_thread is stopped, leader_election is nullptr.
    /// leader_election node in ZK is either deleted, or the session is marked expired.
    void exitLeaderElection();

    /** Selects the parts to merge and writes to the log.
      */
    void mergeSelectingTask();

    /// Checks if some mutations are done and marks them as done.
    void mutationsFinalizingTask();

    /** Write the selected parts to merge into the log,
      * Call when merge_selecting_mutex is locked.
      * Returns false if any part is not in ZK.
      */
    bool createLogEntryToMergeParts(
        zkutil::ZooKeeperPtr & zookeeper,
        const DataPartsVector & parts,
        const String & merged_name,
        bool deduplicate,
        bool force_ttl,
        ReplicatedMergeTreeLogEntryData * out_log_entry = nullptr);

    bool createLogEntryToMutatePart(const MergeTreeDataPart & part, Int64 mutation_version);

    /// Exchange parts.

    /** Returns an empty string if no one has a part.
      */
    String findReplicaHavingPart(const String & part_name, bool active);

    /** Find replica having specified part or any part that covers it.
      * If active = true, consider only active replicas.
      * If found, returns replica name and set 'entry->actual_new_part_name' to name of found largest covering part.
      * If not found, returns empty string.
      */
    String findReplicaHavingCoveringPart(LogEntry & entry, bool active);
    String findReplicaHavingCoveringPart(const String & part_name, bool active, String & found_part_name);

    /** Download the specified part from the specified replica.
      * If `to_detached`, the part is placed in the `detached` directory.
      * If quorum != 0, then the node for tracking the quorum is updated.
      * Returns false if part is already fetching right now.
      */
    bool fetchPart(const String & part_name, const String & replica_path, bool to_detached, size_t quorum);

    /// Required only to avoid races between executeLogEntry and fetchPartition
    std::unordered_set<String> currently_fetching_parts;
    std::mutex currently_fetching_parts_mutex;


    /// With the quorum being tracked, add a replica to the quorum for the part.
    void updateQuorum(const String & part_name);

    /// Creates new block number if block with such block_id does not exist
    std::optional<EphemeralLockInZooKeeper> allocateBlockNumber(
        const String & partition_id, zkutil::ZooKeeperPtr & zookeeper,
        const String & zookeeper_block_id_path = "");

    /** Wait until all replicas, including this, execute the specified action from the log.
      * If replicas are added at the same time, it can not wait the added replica .
      *
      * NOTE: This method must be called without table lock held.
      * Because it effectively waits for other thread that usually has to also acquire a lock to proceed and this yields deadlock.
      * TODO: There are wrong usages of this method that are not fixed yet.
      */
    void waitForAllReplicasToProcessLogEntry(const ReplicatedMergeTreeLogEntryData & entry);

    /** Wait until the specified replica executes the specified action from the log.
      * NOTE: See comment about locks above.
      */
    void waitForReplicaToProcessLogEntry(const String & replica_name, const ReplicatedMergeTreeLogEntryData & entry);

    /// Choose leader replica, send requst to it and wait.
    void sendRequestToLeaderReplica(const ASTPtr & query, const Context & query_context);

    /// Throw an exception if the table is readonly.
    void assertNotReadonly() const;

    /// Produce an imaginary part info covering all parts in the specified partition (at the call moment).
    /// Returns false if the partition doesn't exist yet.
    bool getFakePartCoveringAllPartsInPartition(const String & partition_id, MergeTreePartInfo & part_info);

    /// Check for a node in ZK. If it is, remember this information, and then immediately answer true.
    std::unordered_set<std::string> existing_nodes_cache;
    std::mutex existing_nodes_cache_mutex;
    bool existsNodeCached(const std::string & path);

    /// Remove block IDs from `blocks/` in ZooKeeper for the given partition ID in the given block number range.
    void clearBlocksInPartition(
        zkutil::ZooKeeper & zookeeper, const String & partition_id, Int64 min_block_num, Int64 max_block_num);

    /// Info about how other replicas can access this one.
    ReplicatedMergeTreeAddress getReplicatedMergeTreeAddress() const;

    bool dropPartsInPartition(zkutil::ZooKeeper & zookeeper, String & partition_id,
        StorageReplicatedMergeTree::LogEntry & entry, bool detach);

    /// Find cluster address for host
    std::optional<Cluster::Address> findClusterAddress(const ReplicatedMergeTreeAddress & leader_address) const;

    // Partition helpers
    void clearColumnOrIndexInPartition(const ASTPtr & partition, LogEntry && entry, const Context & query_context);
    void dropPartition(const ASTPtr & query, const ASTPtr & partition, bool detach, const Context & query_context);
    void attachPartition(const ASTPtr & partition, bool part, const Context & query_context);
    void replacePartitionFrom(const StoragePtr & source_table, const ASTPtr & partition, bool replace, const Context & query_context);
    void fetchPartition(const ASTPtr & partition, const String & from, const Context & query_context);

    /// Check granularity of already existing replicated table in zookeeper if it exists
    /// return true if it's fixed
    bool checkFixedGranualrityInZookeeper();

protected:
    /** If not 'attach', either creates a new table in ZK, or adds a replica to an existing table.
      */
    StorageReplicatedMergeTree(
        const String & zookeeper_path_,
        const String & replica_name_,
        bool attach,
        const String & database_name_, const String & name_,
        const ColumnsDescription & columns_,
        const IndicesDescription & indices_,
        const ConstraintsDescription & constraints_,
        Context & context_,
        const String & date_column_name,
        const ASTPtr & partition_by_ast_,
        const ASTPtr & order_by_ast_,
        const ASTPtr & primary_key_ast_,
        const ASTPtr & sample_by_ast_,
        const ASTPtr & table_ttl_ast_,
        const MergingParams & merging_params_,
        std::unique_ptr<MergeTreeSettings> settings_,
        bool has_force_restore_data_flag);
};


extern const int MAX_AGE_OF_LOCAL_PART_THAT_WASNT_ADDED_TO_ZOOKEEPER;

}
