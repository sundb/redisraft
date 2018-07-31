import sys
import time
import os
import os.path
import subprocess
import threading
import random
import logging
import redis

LOG = logging.getLogger('sandbox')

class RedisRaftError(Exception):
    pass

class RedisRaftTimeout(RedisRaftError):
    pass


class PipeLogger(threading.Thread):
    def __init__(self, pipe, prefix):
        super(PipeLogger, self).__init__()
        self.prefix = prefix
        self.pipe = pipe
        self.daemon = True
        self.start()

    def getvalue(self):
        return self.buf

    def run(self):
        for line in iter(self.pipe.readline, b''):
            LOG.debug('{}: {}'.format(self.prefix,
                                      str(line, 'utf-8').rstrip()))

class DefaultConfig(object):
    executable = 'redis-server'
    args = None
    raftmodule = 'redisraft.so'
    up_timeout = 2

class DebugConfig(DefaultConfig):
    executable = 'gnome-terminal'
    args = [
        '--wait', '--', 'gdb', '--args', 'redis-server'
    ]
    up_timeout = None

class ValgrindConfig(DefaultConfig):
    executable = 'valgrind'
    args = [
        '--leak-check=full',
        '--show-reachable=no',
        '--show-possibly-lost=no',
        '--show-reachable=no',
        '--suppressions=../redis/src/valgrind.sup',
        '--log-file=valgrind-redis.%p',
        'redis-server'
    ]

class ValgrindShowPossiblyLostConfig(DefaultConfig):
    executable = 'valgrind'
    args = [
        '--leak-check=full',
        '--show-reachable=no',
        '--show-possibly-lost=yes',
        '--suppressions=../redis/src/valgrind.sup',
        '--log-file=valgrind-redis.%p',
        '--suppressions=redisraft.supp',
        'redis-server'
    ]

def resolve_config():
    name = os.environ.get('SANDBOX_CONFIG')
    if name is not None:
        return getattr(sys.modules[__name__], name)
    return DefaultConfig

class RedisRaft(object):
    def __init__(self, _id, port, config=None, raft_args=None):
        if config is None:
            config = resolve_config()
        if raft_args is None:
            raft_args = {}
        else:
            raft_args = raft_args.copy()
        self.id = _id
        self.port = port
        self.executable = config.executable
        self.process = None
        self.raftlog = 'raftlog{}.db'.format(self.id)
        self.dbfilename = 'redis{}.rdb'.format(self.id)
        self.up_timeout = config.up_timeout
        self.args = config.args.copy() if config.args else []
        self.args += ['--port', str(port),
                      '--dbfilename', self.dbfilename]
        self.args += ['--loadmodule', os.path.abspath(config.raftmodule)]

        raft_args['id'] = str(_id)
        raft_args['addr'] = 'localhost:{}'.format(self.port)
        raft_args['raftlog'] = self.raftlog

        self.raft_args = ['{}={}'.format(k, v) for k, v in raft_args.items()]
        self.client = redis.Redis(host='localhost', port=self.port)
        self.client.connection_pool.connection_kwargs['parser_class'] = \
            redis.connection.PythonParser
        self.client.set_response_callback('raft.info', redis.client.parse_info)
        self.stdout = None
        self.stderr = None
        self.cleanup()

    def init(self):
        self.start(['init'])
        return self

    def join(self, ports):
        self.start(['join=localhost:{}'.format(port) for port in ports])
        return self

    def start(self, extra_raft_args=None):
        if extra_raft_args is None:
            extra_raft_args = []
        args = [self.executable] + self.args + self.raft_args + extra_raft_args
        self.process = subprocess.Popen(
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            executable=self.executable,
            args=args)
        self.stdout = PipeLogger(self.process.stdout,
                                 '{}/stdout'.format(self.id))
        self.stderr = PipeLogger(self.process.stderr,
                                 '{}/stderr'.format(self.id))

        self.verify_up()
        LOG.info('RedisRaft<%s> is up, pid=%s', self.id, self.process.pid)

    def verify_up(self):
        retries = self.up_timeout
        if retries is not None:
            retries *= 100
        while True:
            try:
                self.client.ping()
                return
            except redis.exceptions.ConnectionError:
                if retries is not None:
                    retries -= 1
                    if not retries:
                        LOG.fatal('RedisRaft<%s> failed to start', self.id)
                        raise RuntimeError('RedisRaft<%s> failed to start' %
                                        self.id)
                time.sleep(0.01)

    def terminate(self):
        if self.process:
            try:
                self.process.terminate()
                self.process.wait()

            except OSError as err:
                LOG.error('RedisRaft<%s> failed to terminate: %s',
                          self.id, err)
                pass
            else:
                LOG.info('RedisRaft<%s> terminated', self.id)
        self.process = None

    def restart(self):
        self.terminate()
        self.start()

    def cleanup(self):
        if os.path.exists(self.raftlog):
            os.unlink(self.raftlog)
        if os.path.exists(self.dbfilename):
            os.unlink(self.dbfilename)

    def raft_exec(self, *args):
        cmd = ['RAFT'] + list(args)
        return self.client.execute_command(*cmd)

    def raft_config_set(self, key, val):
        return self.client.execute_command('raft.config', 'set', key, val)

    def raft_info(self):
        return self.client.execute_command('raft.info')

    def commit_index(self):
        return self.raft_info()['commit_index']

    def current_index(self):
        return self.raft_info()['current_index']

    def _wait_for_condition(self, test_func, timeout_func, timeout=3):
        retries = timeout * 10
        while retries > 0:
            try:
                if test_func():
                    return
            except redis.ResponseError as err:
                if not str(err).startswith('UNBLOCKED'):
                    raise

            retries -= 1
            time.sleep(0.1)
        timeout_func()

    def wait_for_election(self, timeout=3):
        def has_leader():
            return bool(self.raft_info()['leader_id'] != -1)
        def raise_no_master_error():
            raise RedisRaftTimeout('No master elected')
        self._wait_for_condition(has_leader, raise_no_master_error, timeout)

    def wait_for_log_applied(self, timeout=3):
        def commit_idx_applied():
            info = self.raft_info()
            return bool(info['commit_index'] == info['last_applied_index'])
        def raise_not_applied():
            raise RedisRaftTimeout('Last committed entry not yet applied')
        self._wait_for_condition(commit_idx_applied, raise_not_applied,
                                 timeout)
        LOG.debug("Finished waiting logs to be applied.")

    def wait_for_commit_index(self, idx, timeout=3):
        def commit_idx_reached():
            info = self.raft_info()
            return bool(info['commit_index'] == idx)
        def raise_not_reached():
            raise RedisRaftTimeout('Expected commit index not reached')
        self._wait_for_condition(commit_idx_reached, raise_not_reached,
                                 timeout)

    def wait_for_num_voting_nodes(self, count, timeout=10):
        def num_voting_nodes_match():
            info = self.raft_info()
            return bool(info['num_voting_nodes'] == count)
        def raise_not_added():
            info = self.raft_info()
            raise RedisRaftTimeout('Nodes not added')
        self._wait_for_condition(num_voting_nodes_match, raise_not_added,
                                 timeout)
        LOG.debug("Finished waiting for num_voting_nodes == %d", count)

    def wait_for_node_voting(self, timeout=10):
        def check_voting():
            info = self.raft_info()
            return bool(info['is_voting'] == 'yes')
        def raise_not_voting():
            info = self.raft_info()
            LOG.debug("Non voting node: %s", str(info))
            raise RedisRaftTimeout('Node is not voting')
        self._wait_for_condition(check_voting, raise_not_voting, timeout)

    def wait_for_info_param(self, name, value, timeout=10):
        def check_param():
            info = self.raft_info()
            return bool(info.get(name) == value)
        def raise_not_matched():
            raise RedisRaftTimeout('RAFT.INFO "%s" did not reach "%s"' %
                                   (name, value))
        self._wait_for_condition(check_param, raise_not_matched, timeout)

    def destroy(self):
        self.terminate()
        self.cleanup()

class Cluster(object):
    noleader_timeout = 5
    base_port = 5000

    def __init__(self):
        self.next_id = 1
        self.nodes = {}
        self.leader = None

    def nodes_count(self):
        return len(self.nodes)

    def node_ports(self):
        return [self.base_port + p for p in self.nodes.keys()]

    def create(self, node_count, raft_args=None):
        if raft_args is None:
            raft_args={}
        assert self.nodes == {}
        self.nodes = {x: RedisRaft(x, self.base_port + x,
                                   raft_args=raft_args)
                      for x in range(1, node_count + 1)}
        self.next_id = node_count + 1
        for _id, node in self.nodes.items():
            if _id == 1:
                node.init()
            else:
                node.join([self.base_port + 1])
        self.leader = 1
        self.node(1).wait_for_num_voting_nodes(len(self.nodes))
        self.node(1).wait_for_log_applied()

    def add_node(self):
        _id = self.next_id
        self.next_id += 1
        node = RedisRaft(_id, self.base_port + _id)
        if len(self.nodes) > 0:
            node.join(self.node_ports())
        else:
            node.init()
            self.leader = _id
        self.nodes[_id] = node
        return node

    def reset_leader(self):
        self.leader = next(iter(self.nodes.keys()))

    def remove_node(self, _id):
        def _func():
            self.node(self.leader).client.execute_command('RAFT.REMOVENODE',
                                                          _id)
        self.raft_retry(_func)
        self.nodes[_id].destroy()
        del(self.nodes[_id])
        if self.leader == _id:
            self.reset_leader()

    def random_node_id(self):
        return random.choice(list(self.nodes.keys()))

    def node(self, _id):
        return self.nodes[_id]

    def leader_node(self):
        return self.nodes[self.leader]

    def exec_all(self, *cmd):
        result = []
        for _id, node in self.nodes.items():
            try:
                r = node.client.execute_command(*cmd)
                result.append(r)
            except redis.ConnectionError:
                pass
        return result

    def wait_for_unanimity(self, exclude=None):
        commit_idx = self.node(self.leader).commit_index()
        for _id, node in self.nodes.items():
            if exclude is not None and int(_id) in exclude:
                continue
            node.wait_for_commit_index(commit_idx)

    def raft_retry(self, func):
        start_time = time.time()
        while time.time() < start_time + self.noleader_timeout:
            try:
                return func()
            except redis.ConnectionError:
                self.leader = self.random_node_id()
            except redis.ResponseError as err:
                if str(err).startswith('UNBLOCKED'):
                    # Ignore unblocked replies...
                    time.sleep(0.5)
                elif str(err).startswith('MOVED'):
                    start_time = time.time()
                    port = int(str(err).split(':')[-1])
                    new_leader = port - 5000
                    assert new_leader != self.leader

                    # When removing a leader there can be a race condition,
                    # in this case we need to do nothing
                    if new_leader in self.nodes:
                        self.leader = new_leader
                elif str(err).startswith('NOLEADER'):
                    time.sleep(0.5)
                else:
                    raise
        raise RedisRaftError('No leader elected')

    def raft_exec(self, *cmd):
        def _func():
            return self.nodes[self.leader].raft_exec(*cmd)
        return self.raft_retry(_func)

    def destroy(self):
        for node in self.nodes.values():
            node.destroy()
