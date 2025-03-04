/*
 * This file is part of RedisRaft.
 *
 * Copyright (c) 2020-2021 Redis Ltd.
 *
 * RedisRaft is licensed under the Redis Source Available License (RSAL).
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>

#include "redisraft.h"

int redis_raft_loglevel = LOGLEVEL_INFO;
RedisModuleCtx *redis_raft_log_ctx = NULL;

const char *redis_raft_log_levels[] = { "warning", "notice", "verbose", "debug" };

RedisRaftCtx redis_raft = { 0 };
static RedisRaftConfig config;

/* This is needed for newer pthread versions to properly link and work */
#ifdef LINUX
void *__dso_handle;
#endif

#define VALID_NODE_ID(x)    ((x) > 0)

/* Parse a node address from a RedisModuleString */
static RRStatus getNodeAddrFromArg(RedisModuleCtx *ctx, RedisModuleString *arg, NodeAddr *addr)
{
    size_t node_addr_len;
    const char *node_addr_str = RedisModule_StringPtrLen(arg, &node_addr_len);
    if (!NodeAddrParse(node_addr_str, node_addr_len, addr)) {
        RedisModule_ReplyWithError(ctx, "invalid node address");
        return RR_ERROR;
    }

    return RR_OK;
}

/* RAFT.NODE ADD [id] [address:port]
 *   Add a new node to the cluster.  The [id] can be an explicit non-zero value,
 *   or zero to let the cluster choose one.
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -CLUSTERDOWN ||
 *   -MOVED <slot> <addr>:<port> ||
 *   *2
 *   :<new node id>
 *   :<dbid>
 *
 * RAFT.NODE REMOVE [id]
 *   Remove an existing node from the cluster.
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -CLUSTERDOWN ||
 *   -MOVED <slot> <addr>:<port> ||
 *   +OK
 */

static int cmdRaftNode(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisRaftCtx *rr = &redis_raft;
    RaftReq *req = NULL;
    size_t cmd_len;

    if (rr->state != REDIS_RAFT_UP) {
        RedisModule_ReplyWithError(ctx, "NOCLUSTER No Raft Cluster");
        return REDISMODULE_OK;
    }

    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);
    if (!strncasecmp(cmd, "ADD", cmd_len)) {
        if (argc != 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        /* Validate node id */
        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK ||
                (node_id && !VALID_NODE_ID(node_id))) {
            RedisModule_ReplyWithError(ctx, "invalid node id");
            return REDISMODULE_OK;
        }

        /* Parse address */
        NodeAddr node_addr = { 0 };
        if (getNodeAddrFromArg(ctx, argv[3], &node_addr) == RR_ERROR) {
            /* Error already produced */
            return REDISMODULE_OK;
        }

        req = RaftReqInit(ctx, RR_CFGCHANGE_ADDNODE);
        req->r.cfgchange.id = node_id;
        req->r.cfgchange.addr = node_addr;
    } else if (!strncasecmp(cmd, "REMOVE", cmd_len)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK ||
            !VALID_NODE_ID(node_id)) {
                RedisModule_ReplyWithError(ctx, "invalid node id");
                return REDISMODULE_OK;
        }

        req = RaftReqInit(ctx, RR_CFGCHANGE_REMOVENODE);
        req->r.cfgchange.id = node_id;
    } else {
        RedisModule_ReplyWithError(ctx, "RAFT.NODE supports ADD / REMOVE only");
        return REDISMODULE_OK;
    }

    RaftReqSubmit(rr, req);
    return REDISMODULE_OK;
}

/* RAFT.TRANSFER_LEADER [target_node_id]
  *   Attempt to transfer raft cluster leadership to targeted node
 * Reply:
 * ???
 */

static int cmdRaftTransferLeader(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) == REDISMODULE_ERR) {
        LOG_ERROR("failed to convert %s", RedisModule_StringPtrLen(argv[1], NULL));
        RedisModule_ReplyWithError(ctx, "invalid target node id");
        return REDISMODULE_OK;
    }

    if (target_node_id == rr->config->id) {
        RedisModule_ReplyWithError(ctx, "can't transfer to self");
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_TRANSFER_LEADER);
    req->r.node_to_transfer_leader = target_node_id;

    RaftReqSubmit(rr, req);
    return REDISMODULE_OK;
}

/* RAFT.TIMEOUT_NOW
 *   instruct this node to force an election
 * Reply:
 * ???
 */

static int cmdRaftTimeoutNow(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;
    if (argc != 1) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_TIMEOUT_NOW);
    RaftReqSubmit(rr, req);

    return REDISMODULE_OK;
}

/* RAFT.REQUESTVOTE [target_node_id] [src_node_id] [term]:[candidate_id]:[last_log_idx]:[last_log_term]:[transfer_leader]
 *   Request a node's vote (per Raft paper).
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   *2
 *   :<term>
 *   :<granted> (0 or 1)
 */

static int cmdRaftRequestVote(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) == REDISMODULE_ERR ||
        target_node_id != rr->config->id) {
            RedisModule_ReplyWithError(ctx, "invalid or incorrect target node id");
            return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_REQUESTVOTE);
    if (RedisModuleStringToInt(argv[2], &req->r.requestvote.src_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid source node id");
        goto error_cleanup;
    }

    size_t tmplen;
    const char *tmpstr = RedisModule_StringPtrLen(argv[3], &tmplen);
    if (sscanf(tmpstr, "%d:%ld:%d:%ld:%ld:%d",
                &req->r.requestvote.msg.prevote,
                &req->r.requestvote.msg.term,
                &req->r.requestvote.msg.candidate_id,
                &req->r.requestvote.msg.last_log_idx,
                &req->r.requestvote.msg.last_log_term,
                &req->r.requestvote.msg.transfer_leader) != 6) {
        RedisModule_ReplyWithError(ctx, "invalid message");
        goto error_cleanup;
    }

    RaftReqSubmit(rr, req);
    return REDISMODULE_OK;

error_cleanup:
    RaftReqFree(req);
    return REDISMODULE_OK;
}

/* RAFT [Redis command to execute]
 *   Submit a Redis command to be appended to the Raft log and applied.
 *   The command blocks until it has been committed to the log by the majority
 *   and applied locally.
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -CLUSTERDOWN ||
 *   -MOVED <slot> <addr>:<port> ||
 *   Any standard Redis reply, depending on the command.
 */

static int cmdRaft(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_REDISCOMMAND);
    RaftRedisCommand *cmd = RaftRedisCommandArrayExtend(&req->r.redis.cmds);

    cmd->argc = argc - 1;
    cmd->argv = RedisModule_Alloc((argc - 1) * sizeof(RedisModuleString *));

    int i;
    for (i = 0; i < argc - 1; i++) {
        /* TODO: We currently duplicate the string instead of using RetainString
         * due to a problem introduced by https://github.com/redis/redis/pull/5834
         * where a retained string may still be mutated upon returning to Redis,
         * so we end up with a race that can result with corruption before we
         * serialize the request into a log entry entry.
         *
         * Revisit this if/when the Module API addresses this issue in a better way.
         */
        cmd->argv[i] = RedisModule_CreateStringFromString(NULL, argv[i + 1]);
    }
    RaftReqSubmit(&redis_raft, req);

    return REDISMODULE_OK;
}

/* RAFT.ENTRY [Serialized Entry]
 *   Receive a serialized batch of Redis commands (like a Raft entry) and
 *   process them, as if received as individual RAFT commands.
 *
 *   This is used to simplify the proxying of MULTI/EXEC commands.
 * Reply:
 *   -MOVED <addr> ||
 *   Any standard Redis reply
 */
static int cmdRaftEntry(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t data_len;
    const char *data = RedisModule_StringPtrLen(argv[1], &data_len);

    RaftReq *req = RaftReqInit(ctx, RR_REDISCOMMAND);
    if (RaftRedisCommandArrayDeserialize(&req->r.redis.cmds, data, data_len) != RR_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid argument");
        RaftReqFree(req);
    } else {
        RaftReqSubmit(&redis_raft, req);
    }

    return REDISMODULE_OK;
}

/* RAFT.INFO
 *   Display Raft module specific info.
 * Reply:
 *   Raw text output, formatted like INFO.
 */
static int cmdRaftInfo(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RaftReq *req = RaftReqInit(ctx, RR_INFO);
    RaftReqSubmit(&redis_raft, req);

    return REDISMODULE_OK;
}


/* RAFT.AE [target_node_id] [src_node_id] [term]:[prev_log_idx]:[prev_log_term]:[leader_commit]
 *      [n_entries] [<term>:<id>:<type> <entry>]...
 *   A leader request to append entries to the Raft log (per Raft paper).
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   *4
 *   :<term>
 *   :<success> (0 or 1)
 *   :<current_idx>
 *   :<first_idx>
 */

static int cmdRaftAppendEntries(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) == REDISMODULE_ERR ||
        target_node_id != rr->config->id) {
            RedisModule_ReplyWithError(ctx, "invalid or incorrect target node id");
            return REDISMODULE_OK;
    }

    long long n_entries;
    if (RedisModule_StringToLongLong(argv[4], &n_entries) != REDIS_OK) {
        RedisModule_ReplyWithError(ctx, "invalid n_entries value");
        return REDISMODULE_OK;
    }
    if (argc != 5 + 2 * n_entries) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_APPENDENTRIES);
    if (RedisModuleStringToInt(argv[2], &req->r.appendentries.src_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid source node id");
        goto error_cleanup;
    }

    size_t tmplen;
    const char *tmpstr = RedisModule_StringPtrLen(argv[3], &tmplen);
    if (sscanf(tmpstr, "%d:%ld:%ld:%ld:%ld:%lu",
                &req->r.appendentries.msg.leader_id,
                &req->r.appendentries.msg.term,
                &req->r.appendentries.msg.prev_log_idx,
                &req->r.appendentries.msg.prev_log_term,
                &req->r.appendentries.msg.leader_commit,
                &req->r.appendentries.msg.msg_id) != 6) {
        RedisModule_ReplyWithError(ctx, "invalid message");
        goto error_cleanup;
    }

    req->r.appendentries.msg.n_entries = (int)n_entries;
    if (n_entries > 0) {
        req->r.appendentries.msg.entries = RedisModule_Calloc(n_entries, sizeof(msg_entry_t));
    }

    for (int i = 0; i < n_entries; i++) {
        /* Create entry with payload */
        tmpstr = RedisModule_StringPtrLen(argv[6 + 2*i], &tmplen);
        msg_entry_t *e = raft_entry_new(tmplen);
        memcpy(e->data, tmpstr, tmplen);

        /* Parse additional entry fields */
        tmpstr = RedisModule_StringPtrLen(argv[5 + 2*i], &tmplen);
        if (sscanf(tmpstr, "%ld:%d:%hd",
                    &e->term,
                    &e->id,
                    &e->type) != 3) {
            RedisModule_ReplyWithError(ctx, "invalid entry");
            raft_entry_release(e);
            goto error_cleanup;
        }

        req->r.appendentries.msg.entries[i] = e;
    }

    RaftReqSubmit(rr, req);
    return REDISMODULE_OK;

error_cleanup:
    RaftReqFree(req);
    return REDISMODULE_OK;
}

/* RAFT.CONFIG GET [wildcard]
 *   Query Raft configuration parameters.
 *
 * RAFT.CONFIG SET [param] [value]
 *   Set a Raft configuration parameter.
 *
 * This is basically identical to Redis CONFIG GET / CONFIG SET, for
 * Raft specific configuration.
 */

static int cmdRaftConfig(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);
    if (!strncasecmp(cmd, "SET", cmd_len) && argc >= 4) {
        handleConfigSet(rr, ctx, argv, argc);
        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "GET", cmd_len) && argc == 3) {
        handleConfigGet(ctx, rr->config, argv, argc);
        return REDISMODULE_OK;
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Unknown RAFT.CONFIG subcommand or wrong number of arguments");
    }

    return REDISMODULE_OK;
}

/* RAFT.SNAPSHOT [target-node-id] [src_node_id]
 *               [term]:[leader_id]:[msg_id]:[snapshot_index]:[snapshot_term]:[chunk_offset]:[last_chunk]
 *               [chunk_data]
 *   Store the specified snapshot chunk (e.g. Raft paper's InstallSnapshot RPC).
 *
 *  Reply:
 *    -NOCLUSTER ||
 *    -LOADING ||
 *    *5
 *    :<term>
 *    :<msg_id>
 *    :<offset>
 *    :<success>
 *    :<last_chunk>
 */

static int cmdRaftSnapshot(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) != REDISMODULE_OK ||
        target_node_id != rr->config->id) {
            RedisModule_ReplyWithError(ctx, "ERR invalid or incorrect target node id");
            return REDISMODULE_OK;
        }

    RaftReq *req = RaftReqInit(ctx, RR_SNAPSHOT);
    if (RedisModuleStringToInt(argv[2], &req->r.snapshot.src_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid source node id");
        goto error;
    }

    size_t tmplen;
    const char *tmpstr = RedisModule_StringPtrLen(argv[3], &tmplen);

    if (sscanf(tmpstr, "%lu:%d:%lu:%lu:%lu:%llu:%d",
               &req->r.snapshot.msg.term,
               &req->r.snapshot.msg.leader_id,
               &req->r.snapshot.msg.msg_id,
               &req->r.snapshot.msg.snapshot_index,
               &req->r.snapshot.msg.snapshot_term,
               &req->r.snapshot.msg.chunk.offset,
               &req->r.snapshot.msg.chunk.last_chunk) != 7) {
        RedisModule_ReplyWithError(ctx, "invalid message");
        goto error;
    }

    /* TODO: We currently duplicate the string instead of using RetainString
     * due to a problem introduced by https://github.com/redis/redis/pull/5834
     * where a retained string may still be mutated upon returning to Redis,
     * so we end up with a race that can result with corruption before we
     * serialize the request into a log entry entry.
     *
     * Revisit this if/when the Module API addresses this issue in a better way.
     */
    req->r.snapshot.data = RedisModule_CreateStringFromString(NULL, argv[4]);

    size_t len;
    void *data = (void*) RedisModule_StringPtrLen(req->r.snapshot.data, &len);

    req->r.snapshot.msg.chunk.data = data;
    req->r.snapshot.msg.chunk.len = len;

    RaftReqSubmit(rr, req);
    return REDISMODULE_OK;

error:
    RaftReqFree(req);
    return REDISMODULE_OK;
}

/* RAFT.CLUSTER INIT
 *   Initializes a new Raft cluster.
 * Reply:
 *   +OK [dbid]
 *
 * RAFT.CLUSTER JOIN [addr:port]
 *   Join an existing cluster.
 *   The operation is asynchronous and may take place/retry in the background.
 * Reply:
 *   +OK
 */

static int cmdRaftCluster(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RaftReq *req = NULL;
    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);
    if (!strncasecmp(cmd, "INIT", cmd_len)) {
        if (argc != 2) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }
        req = RaftReqInit(ctx, RR_CLUSTER_INIT);
    } else if (!strncasecmp(cmd, "JOIN", cmd_len)) {
        if (argc < 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        int i;
        req = RaftReqInit(ctx, RR_CLUSTER_JOIN);

        for (i = 2; i < argc; i++) {
            NodeAddr addr;
            if (getNodeAddrFromArg(ctx, argv[i], &addr) == RR_ERROR) {
                /* Error already produced */
                return REDISMODULE_OK;
            }
            NodeAddrListAddElement(&req->r.cluster_join.addr, &addr);
        }
    } else {
        RedisModule_ReplyWithError(ctx, "RAFT.CLUSTER supports INIT / JOIN only");
        return REDISMODULE_OK;
    }

    RaftReqSubmit(rr, req);
    return REDISMODULE_OK;
}

/* RAFT.SHARDGROUP GET
 *   Returns the current cluster's local shard group configuration, in a format
 *   that is compatible with RAFT.SHARDGROUP ADD.
 * Reply:
 *   [start-slot] [end-slot] [node-id node-addr] [node-id node-addr...]
 *
 * RAFT.SHARDGROUP ADD [start-slot] [end-slot] [node-id node-addr] [node-id node-addr ...]
 *   Adds a new shard group configuration.
 * Reply:
 *   +OK
 *
 * RAFT.SHARDGROUP LINK [node-addr:port]
 *   Link cluster with a new remote shardgroup.
 * Reply:
 *   +OK
 *   -ERR error description
 */

static int cmdRaftShardGroup(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RaftReq *req;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (!redis_raft.config->sharding) {
        RedisModule_ReplyWithError(ctx, "ERR: RedisRaft sharding not enabled");
        return REDISMODULE_OK;
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    if (!strncasecmp(cmd, "GET", cmd_len)) {
        req = RaftReqInit(ctx, RR_SHARDGROUP_GET);
        RaftReqSubmit(&redis_raft, req);

        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "ADD", cmd_len)) {
        if (argc < 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        req = RaftReqInit(ctx, RR_SHARDGROUP_ADD);
        if (ShardGroupParse(ctx, &argv[2], argc - 2, &req->r.shardgroup_add) != RR_OK) {
            /* Error reply already produced by parseShardGroupFromArgs */
            RaftReqFree(req);
            return REDISMODULE_OK;
        }

        RaftReqSubmit(&redis_raft, req);
        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "UPDATE", cmd_len)) {
        if (argc < 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        req = RaftReqInit(ctx, RR_SHARDGROUP_UPDATE);
        if (ShardGroupParse(ctx, &argv[2], argc - 2, &req->r.shardgroup_add) != RR_OK) {
            /* Error reply already produced by parseShardGroupFromArgs */
            RaftReqFree(req);
            return REDISMODULE_OK;
        }

        RaftReqSubmit(&redis_raft, req);
        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "LINK", cmd_len)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        size_t len;
        const char *str = RedisModule_StringPtrLen(argv[2], &len);

        req = RaftReqInit(ctx, RR_SHARDGROUP_LINK);
        if (!NodeAddrParse(str, len, &req->r.shardgroup_link.addr)) {
            RedisModule_ReplyWithError(ctx, "invalid address/port specified") ;
            RaftReqFree(req);
            return REDISMODULE_OK;
        }

        RaftReqSubmit(&redis_raft, req);
        return REDISMODULE_OK;
    } else {
        RedisModule_ReplyWithError(ctx, "RAFT.SHARDGROUP supports GET / ADD / LINK only");
        return REDISMODULE_OK;
    }
}


/* RAFT.DEBUG COMPACT [delay]
 *   Initiate an immediate rewrite of the Raft log + snapshot.
 *   If [delay] is specified, introduce an artificial delay of [delay] seconds in
 *   the background rewrite child process.
 * Reply:
 *   +OK
 */
static int cmdRaftDebug(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t cmdlen;
    const char *cmdstr = RedisModule_StringPtrLen(argv[1], &cmdlen);
    char cmd[cmdlen+1];

    memcpy(cmd, cmdstr, cmdlen);
    cmd[cmdlen] = '\0';

    if (!strncasecmp(cmd, "compact", cmdlen)) {
        long long delay = 0;
        if (argc == 3) {
            if (RedisModule_StringToLongLong(argv[2], &delay) != REDISMODULE_OK) {
                RedisModule_ReplyWithError(ctx, "ERR invalid compact delay value");
                return REDISMODULE_OK;
            }
        }

        RaftReq *req = RaftDebugReqInit(ctx, RR_DEBUG_COMPACT);
        req->r.debug.d.compact.delay = delay;
        RaftReqSubmit(&redis_raft, req);
    } else if (!strncasecmp(cmd, "nodecfg", cmdlen)) {
        if (argc != 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, "ERR invalid node id");
            return REDISMODULE_OK;
        }

        size_t slen;
        const char *str = RedisModule_StringPtrLen(argv[3], &slen);

        RaftReq *req = RaftDebugReqInit(ctx, RR_DEBUG_NODECFG);
        req->r.debug.d.nodecfg.id = node_id;
        req->r.debug.d.nodecfg.str = RedisModule_Alloc(slen + 1);
        memcpy(req->r.debug.d.nodecfg.str, str, slen);
        req->r.debug.d.nodecfg.str[slen] = '\0';
        RaftReqSubmit(&redis_raft, req);
    } else if (!strncasecmp(cmd, "sendsnapshot", cmdlen)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, "ERR invalid node id");
            return REDISMODULE_OK;
        }

        RaftReq *req = RaftDebugReqInit(ctx, RR_DEBUG_SENDSNAPSHOT);
        req->r.debug.d.sendsnapshot.id = node_id;
        RaftReqSubmit(&redis_raft, req);
    } else if (!strncasecmp(cmd, "used_node_ids", cmdlen)) {
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

        int len = 0;
        for (NodeIdEntry *e = redis_raft.snapshot_info.used_node_ids; e != NULL; e = e->next) {
            RedisModule_ReplyWithLongLong(ctx, e->id);
            len++;
        }

        RedisModule_ReplySetArrayLength(ctx, len);
    } else if (!strncasecmp(cmd, "exec", cmdlen) && argc > 3) {
        size_t exec_cmd_len;
        const char *exec_cmd = RedisModule_StringPtrLen(argv[2], &exec_cmd_len);

        RedisModuleCallReply *reply = RedisModule_Call(ctx, exec_cmd, "v", &argv[3], argc - 3);
        if (!reply) {
            RedisModule_ReplyWithError(ctx, "Bad command or failed to execute");
        } else {
            RedisModule_ReplyWithCallReply(ctx, reply);
            RedisModule_FreeCallReply(reply);
        }
    } else {
        RedisModule_ReplyWithError(ctx, "ERR invalid debug subcommand");
    }

    return REDISMODULE_OK;
}

static int cmdRaftNodeShutdown(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long node_id;
    if (RedisModule_StringToLongLong(argv[1], &node_id) != REDISMODULE_OK ||
        (node_id && !VALID_NODE_ID(node_id))) {
        RedisModule_ReplyWithError(ctx, "invalid node id");
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_NODE_SHUTDOWN);
    req->r.node_shutdown.id = (raft_node_id_t) node_id;

    RaftReqSubmit(rr, req);

    return REDISMODULE_OK;
}

static void handleClientDisconnect(RedisModuleCtx *ctx,
        RedisModuleEvent eid, uint64_t subevent, void *data)
{
    if (eid.id == REDISMODULE_EVENT_CLIENT_CHANGE &&
            subevent == REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED) {
        RedisModuleClientInfo *ci = (RedisModuleClientInfo *) data;
        RaftReq *req = RaftReqInit(NULL, RR_CLIENT_DISCONNECT);
        req->r.client_disconnect.client_id = ci->id;
        RaftReqSubmit(&redis_raft, req);
    }
}

/* Command filter callback that intercepts normal Redis commands and prefixes them
 * with a RAFT command prefix in order to divert them to execute inside RedisRaft.
 */
static void interceptRedisCommands(RedisModuleCommandFilterCtx *filter)
{
    /* If we're intercepting an RM_Call() processing a Raft entry,
     * skip.
     */
    if (checkInRedisModuleCall()) {
        return;
    }

    const CommandSpec *cs = CommandSpecGet(RedisModule_CommandFilterArgGet(filter, 0));
    if (cs && (cs->flags & CMD_SPEC_DONT_INTERCEPT))
        return;

    /* Prepend RAFT to the original command */
    RedisModuleString *raft_str = RedisModule_CreateString(NULL, "RAFT", 4);
    RedisModule_CommandFilterArgInsert(filter, 0, raft_str);
}


static int registerRaftCommands(RedisModuleCtx *ctx)
{
    /* Register commands.
     *
     * NOTE: Internal RedisRaft module commands must also be set with
     * their apropriate flags in commands.c, typically with a
     * CMD_SPEC_DONT_INTERCEPT flag.
     * */
    if (RedisModule_CreateCommand(ctx, "raft",
                cmdRaft, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.entry",
                cmdRaftEntry, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.info",
                cmdRaftInfo, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.config",
                cmdRaftConfig, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.cluster",
                cmdRaftCluster, "admin",0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.shardgroup",
                cmdRaftShardGroup, "admin",0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.node",
                cmdRaftNode, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.ae",
                cmdRaftAppendEntries, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.transfer_leader",
                                  cmdRaftTransferLeader, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.timeout_now",
                                  cmdRaftTimeoutNow, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.requestvote",
                cmdRaftRequestVote, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.snapshot",
                cmdRaftSnapshot, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.debug",
                cmdRaftDebug, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.nodeshutdown",
                cmdRaftNodeShutdown, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if ((RedisRaftType = RedisModule_CreateDataType(ctx, REDIS_RAFT_DATATYPE_NAME, REDIS_RAFT_DATATYPE_ENCVER,
            &RedisRaftTypeMethods)) == NULL) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

__attribute__((__unused__)) int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (RedisModule_Init(ctx, "redisraft", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, REDIS_NOTICE, "RedisRaft version %s [%s]",
            REDISRAFT_VERSION,
            REDISRAFT_GIT_SHA1);

    /* Sanity check Redis version */
    if (RedisModule_SubscribeToServerEvent == NULL ||
            RedisModule_RegisterCommandFilter == NULL ||
            RedisModule_GetCommandKeys == NULL ||
            RedisModule_GetDetachedThreadSafeContext == NULL) {
        RedisModule_Log(ctx, REDIS_NOTICE, "Redis Raft requires Redis 6.0.9 or newer!");
        return REDISMODULE_ERR;
    }

    /* Sanity check that not running with cluster_enabled */
    RedisModuleServerInfoData *info = RedisModule_GetServerInfo(ctx, "cluster");
    int cluster_enabled = (int) RedisModule_ServerInfoGetFieldSigned(info, "cluster_enabled", NULL);
    RedisModule_FreeServerInfo(ctx, info);
    if (cluster_enabled) {
        RedisModule_Log(ctx, REDIS_NOTICE, "Redis Raft requires Redis not be started with cluster_enabled!");
        return REDISMODULE_ERR;
    }

    /* Create a logging context */
    redis_raft_log_ctx = RedisModule_GetDetachedThreadSafeContext(ctx);

    /* Report arguments */
    size_t str_len = 1024;
    char *str = RedisModule_Calloc(1, str_len);
    int i;
    for (i = 0; i < argc; i++) {
        size_t slen;
        const char *s = RedisModule_StringPtrLen(argv[i], &slen);
        str = catsnprintf(str, &str_len, "%s%.*s", i == 0 ? "" : " ", (int) slen, s);
    }

    RedisModule_Log(ctx, REDIS_NOTICE, "RedisRaft starting, arguments: %s", str);
    RedisModule_Free(str);

    /* Initialize and validate configuration */
    ConfigInit(ctx, &config);
    if (ConfigParseArgs(ctx, argv, argc, &config) == RR_ERROR) {
        return REDISMODULE_ERR;
    }

    /* Configure Redis */
    if (ConfigureRedis(ctx) == RR_ERROR) {
        RedisModule_Log(ctx, REDIS_WARNING, "Failed to set Redis configuration!");
        return REDISMODULE_ERR;
    }

    if (CommandSpecInit(ctx, &config) == RR_ERROR) {
        RedisModule_Log(ctx, REDIS_WARNING, "Failed to initialize internal command table");
        return REDISMODULE_ERR;
    }

    if (registerRaftCommands(ctx) == RR_ERROR) {
        RedisModule_Log(ctx, REDIS_WARNING, "Failed to register commands");
        return REDISMODULE_ERR;
    }

    raft_set_heap_functions(RedisModule_Alloc,
                            RedisModule_Calloc,
                            RedisModule_Realloc,
                            RedisModule_Free);
    uv_replace_allocator(RedisModule_Alloc,
                         RedisModule_Realloc,
                         RedisModule_Calloc,
                         RedisModule_Free);

    if (RedisRaftInit(ctx, &redis_raft, &config) == RR_ERROR) {
        return REDISMODULE_ERR;
    }

    redis_raft.registered_filter = RedisModule_RegisterCommandFilter(ctx,
        interceptRedisCommands, REDISMODULE_CMDFILTER_NOSELF);

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ClientChange,
                handleClientDisconnect) != REDISMODULE_OK) {
        RedisModule_Log(ctx, REDIS_WARNING, "Failed to subscribe to server events.");
        return REDISMODULE_ERR;
    }

    /* Start Raft thread */
    if (RedisRaftStart(ctx, &redis_raft) == RR_ERROR) {
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, REDIS_VERBOSE, "Raft module loaded, state is '%s'",
            getStateStr(&redis_raft));
    return REDISMODULE_OK;
}
