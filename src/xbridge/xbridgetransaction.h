//*****************************************************************************
//*****************************************************************************

#ifndef XBRIDGETRANSACTION_H
#define XBRIDGETRANSACTION_H

#include "uint256.h"
#include "xbridgetransactionmember.h"
#include "xkey.h"
#include "xbridgedef.h"

#include <vector>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/date_time/posix_time/ptime.hpp>

//******************************************************************************
//******************************************************************************
namespace xbridge
{

//*****************************************************************************
//*****************************************************************************
class Transaction
{
public:
    // see strState when editing
    enum State
    {
        trInvalid = 0,
        trNew,
        trJoined,
        trHold,
        trInitialized,
        trCreated,
        trSigned,
        trCommited,

        trConfirmed,
        trFinished,
        trCancelled,
        trDropped
    };

    enum
    {
        // transaction lock time base, in seconds, 10 min
        lockTime = 600,

        // pending transaction ttl in seconds, 72 hours
        pendingTTL = 259200,

        // transaction ttl in seconds, 60 min
        TTL = 3600
    };

public:
    /**
     * @brief Transaction
     */
    Transaction();
    /**
     * @brief Transaction
     * @param id
     * @param sourceAddr
     * @param sourceCurrency
     * @param sourceAmount
     * @param destAddr
     * @param destCurrency
     * @param destAmount
     */
    Transaction(const uint256                    & id,
                const std::vector<unsigned char> & sourceAddr,
                const std::string                & sourceCurrency,
                const uint64_t                   & sourceAmount,
                const std::vector<unsigned char> & destAddr,
                const std::string                & destCurrency,
                const uint64_t                   & destAmount);
    ~Transaction();

    /**
     * @brief id
     * @return id of transaction
     */
    uint256 id() const;


    /**
     * @brief state
     * @return state of transaction
     */
    State state() const;

    /**
     * @brief increaseStateCounter update state counter and update state
     * @param state
     * @param from
     * @return new state of transaction
     */
    State increaseStateCounter(const State state, const std::vector<unsigned char> & from);

    static std::string strState(const State state);
    /**
     * @brief strState
     * @return string description transaction
     */
    std::string strState() const;

    /**
     * @brief updateTimestamp - update creation time of transaction
     */
    void updateTimestamp();
    /**
     * @brief createdTime
     * @return the creation time of transaction
     */
    boost::posix_time::ptime createdTime() const;

    /**
     * @brief isFinished
     * @return
     */
    bool isFinished() const;
    /**
     * @brief isValid
     * @return
     */
    bool isValid() const;
    /**
     * @brief isExpired
     * @return
     */
    bool isExpired() const;

    /**
     * @brief cancel
     */
    void cancel();
    /**
     * @brief drop
     */
    void drop();
    /**
     * @brief finish
     */
    void finish();

    /**
     * @brief confirm
     * @param id
     * @return
     */
    bool confirm(const std::string & id);

    // hash of transaction
    /**
     * @brief hash1
     * @return hash of transaction
     */
    uint256 hash1() const;
    /**
     * @brief hash2
     * @return hash of transaction
     */
    uint256 hash2() const;

    // uint256                    firstId() const;
    /**
     * @brief a_address
     * @return address of source
     */
    std::vector<unsigned char> a_address() const;

    std::vector<unsigned char> a_destination() const;
    std::string                a_currency() const;
    uint64_t                   a_amount() const;
    std::string                a_payTx() const;
    std::string                a_refTx() const;
    std::string                a_bintxid() const;

    // TODO remove script
    std::vector<unsigned char> a_innerScript() const;

    uint256                    a_datatxid() const;
    std::vector<unsigned char> a_pk1() const;

    // uint256                    secondId() const;
    std::vector<unsigned char> b_address() const;
    std::vector<unsigned char> b_destination() const;
    std::string                b_currency() const;
    uint64_t                   b_amount() const;
    std::string                b_payTx() const;
    std::string                b_refTx() const;
    std::string                b_bintxid() const;

    // TODO remove script
    std::vector<unsigned char> b_innerScript() const;

    // uint256                    b_datatxid() const;
    std::vector<unsigned char> b_pk1() const;

    bool tryJoin(const TransactionPtr other);

    bool                       setKeys(const std::vector<unsigned char> & addr,
                                       const uint256 & datatxid,
                                       const std::vector<unsigned char> & pk);
    bool                       setBinTxId(const std::vector<unsigned char> &addr,
                                          const std::string & id,
                                          const std::vector<unsigned char> & innerScript);

public:
    boost::mutex               m_lock;

private:
    uint256                    m_id;

    boost::posix_time::ptime   m_created;

    State                      m_state;

    bool                       m_a_stateChanged;
    bool                       m_b_stateChanged;

    unsigned int               m_confirmationCounter;

    std::string                m_sourceCurrency;
    std::string                m_destCurrency;

    uint64_t                   m_sourceAmount;
    uint64_t                   m_destAmount;

    std::string                m_bintxid1;
    std::string                m_bintxid2;

    std::vector<unsigned char> m_innerScript1;
    std::vector<unsigned char> m_innerScript2;

    XBridgeTransactionMember   m_a;
    XBridgeTransactionMember   m_b;

    uint256                    m_a_datatxid;
    uint256                    m_b_datatxid;

    std::vector<unsigned char> m_a_pk1;
    std::vector<unsigned char> m_b_pk1;
};

} // namespace xbridge

#endif // XBRIDGETRANSACTION_H
