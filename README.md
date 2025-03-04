# RedisRaft

> :warning: RedisRaft is still being developed and is not yet ready for any real production use. Please do not use it for any mission critical purpose at this time.

### Strongly-Consistent Redis Deployments

RedisRaft is a Redis module that implements the [Raft Consensus
Algorithm](https://raft.github.io/), making it possible to create strongly-consistent clusters of Redis servers.

The Raft algorithm is provided by a [standalone Raft
library](https://github.com/willemt/raft) by Willem-Hendrik Thiart.

## Main Features

* Strong consistency (in the language of [CAP](https://en.wikipedia.org/wiki/CAP_theorem), this system prioritizes consistency and partition-tolerance).
* Support for most Redis data types and commands
* Dynamic cluster configuration (adding / removing nodes)
* Snapshots for log compaction
* Configurable quorum or fast reads

## Getting Started

### Building

To compile the module, you will need:
* Build essentials (a compiler, GNU make, etc.)
* CMake
* GNU autotools (autoconf, automake, libtool).

To build, simply run:

    make

### Creating a RedisRaft Cluster

Note: RedisRaft requires Redis 6.0 or above.

To create a three-node cluster, start the first node:

    redis-server \
        --port 5001 --dbfilename raft1.rdb \
        --loadmodule <path-to>/redisraft.so \
            raft-log-filename raftlog1.db addr localhost:5001

Then initialize the cluster:

    redis-cli -p 5001 raft.cluster init

Now start the second node, and run the `RAFT.CLUSTER JOIN` command to join it to the existing cluster:

    redis-server \
        --port 5002 --dbfilename raft2.rdb \
        --loadmodule <path-to>/redisraft.so \
            raft-log-filename raftlog2.db addr localhost:5002

    redis-cli -p 5002 RAFT.CLUSTER JOIN localhost:5001

Now add the third node in the same way:

    redis-server \
        --port 5003 --dbfilename raft3.rdb \
        --loadmodule <path-to>/redisraft.so \
            raft-log-filename raftlog3.db addr localhost:5003

    redis-cli -p 5003 RAFT.CLUSTER JOIN localhost:5001

To query the cluster state, run the `RAFT.INFO` command:

    redis-cli --raw -p 5001 RAFT.INFO

Now you can start using this RedisRaft cluster. All [supported Redis commands](docs/Using.md) will be executed in a strongly-consistent manner using the Raft protocol.

## Documentation

Please consult the [documentation](docs/TOC.md) for more information.

## License

RedisRaft is licensed under the [Redis Source Available License (RSAL)](LICENSE.rsal).
