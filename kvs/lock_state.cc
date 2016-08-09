// Copyright (c) 2016, Robert Escriva
// All rights reserved.

// e
#include <e/strescape.h>

// BusyBee
#include <busybee_constants.h>

// Google Log
#include <glog/logging.h>

// consus
#include "common/network_msgtype.h"
#include "kvs/configuration.h"
#include "kvs/daemon.h"
#include "kvs/lock_state.h"

using consus::lock_state;

struct lock_state::request
{
    request() : id(), nonce(), txid() {}
    request(comm_id i, uint64_t n, const transaction_id& x)
        : id(i), nonce(n), txid(x) {}
    ~request() throw () {}
    comm_id id;
    uint64_t nonce;
    transaction_id txid;
};

lock_state :: lock_state(const table_key_pair& tk)
    : m_state_key(tk)
    , m_mtx()
    , m_init(false)
    , m_holder()
    , m_reqs()
{
}

lock_state :: ~lock_state() throw ()
{
}

consus :: table_key_pair
lock_state :: state_key()
{
    return m_state_key;
}

bool
lock_state :: finished()
{
    po6::threads::mutex::hold hold(&m_mtx);
    return !m_init || (m_reqs.empty() && m_holder == transaction_id());
}

void
lock_state :: enqueue_lock(comm_id id, uint64_t nonce,
                           const transaction_id& txid,
                           daemon* d)
{
    po6::threads::mutex::hold hold(&m_mtx);

    if (!ensure_initialized(d))
    {
        return;
    }

    m_reqs.push_back(request(id, nonce, txid));

    if (m_holder == transaction_id())
    {
        consus_returncode rc = d->m_data->write_lock(m_state_key.table,
                                                     m_state_key.key,
                                                     txid);

        if (rc != CONSUS_SUCCESS)
        {
            LOG(ERROR) << "failed lock(\""
                       << e::strescape(m_state_key.table)
                       << "\", \""
                       << e::strescape(m_state_key.key)
                       << "\") nonce=" << nonce;
            return;
        }

        send_response(id, nonce, txid, d);
    }
}

void
lock_state :: unlock(comm_id id, uint64_t nonce,
                     const transaction_id& txid,
                     daemon* d)
{
    po6::threads::mutex::hold hold(&m_mtx);

    if (!ensure_initialized(d))
    {
        return;
    }

    if (m_holder == txid)
    {
        assert(!m_reqs.empty());
        assert(m_reqs.front().txid == txid);
        request next;

        if (m_reqs.size() > 1)
        {
            std::list<request>::iterator it = m_reqs.begin();
            ++it;
            assert(it != m_reqs.end());
            next = *it;
        }

        consus_returncode rc = d->m_data->write_lock(m_state_key.table,
                                                     m_state_key.key,
                                                     next.txid);

        if (rc != CONSUS_SUCCESS)
        {
            LOG(ERROR) << "failed unlock(\""
                       << e::strescape(m_state_key.table)
                       << "\", \""
                       << e::strescape(m_state_key.key)
                       << "\") nonce=" << nonce;
            return;
        }

        m_reqs.pop_front();
        m_holder = next.txid;
        send_response(id, nonce, txid, d);

        if (next.txid != transaction_id())
        {
            send_response(next.id, next.nonce, next.txid, d);
        }
    }
}

bool
lock_state :: ensure_initialized(daemon* d)
{
    transaction_id txid;
    consus_returncode rc = d->m_data->read_lock(m_state_key.table,
                                                m_state_key.key,
                                                &txid);

    if (rc != CONSUS_SUCCESS && rc != CONSUS_NOT_FOUND)
    {
        LOG(ERROR) << "failed to initialize lock (\""
                   << e::strescape(m_state_key.table)
                   << "\", \""
                   << e::strescape(m_state_key.key)
                   << "\")";
        return false;
    }

    if (txid != transaction_id())
    {
        m_reqs.push_back(request(comm_id(), 0, txid));
        m_holder = txid;
    }

    m_init = true;
    return true;
}

void
lock_state :: send_response(comm_id id, uint64_t nonce,
                            const transaction_id& txid, daemon* d)
{
    if (id == comm_id())
    {
        return;
    }

    configuration* c = d->get_config();
    replica_set rs;

    if (!c->hash(d->m_us.dc, m_state_key.table, m_state_key.key, &rs))
    {
        return;
    }

    const size_t sz = BUSYBEE_HEADER_SIZE
                    + pack_size(KVS_RAW_LK_RESP)
                    + sizeof(uint64_t)
                    + pack_size(txid)
                    + pack_size(rs);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE)
        << KVS_RAW_LK_RESP << nonce << txid << rs;
    d->send(id, msg);
}
