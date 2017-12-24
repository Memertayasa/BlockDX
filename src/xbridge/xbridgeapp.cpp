//*****************************************************************************
//*****************************************************************************

#include "xbridgeapp.h"
#include "xbridgeexchange.h"
#include "util/xutil.h"
#include "util/logger.h"
#include "util/settings.h"
#include "util/xbridgeerror.h"
#include "version.h"
#include "config.h"
#include "xuiconnector.h"
#include "rpcserver.h"
#include "net.h"
#include "util.h"
#include "xkey.h"
#include "ui_interface.h"
#include "init.h"
#include "wallet.h"
#include "xbridgewalletconnector.h"
#include "xbridgewalletconnectorbtc.h"
#include "xbridgewalletconnectorbcc.h"

#include <assert.h>

#include <boost/chrono/chrono.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <openssl/rand.h>
#include <openssl/md5.h>

//*****************************************************************************
//*****************************************************************************
XUIConnector xuiConnector;

//*****************************************************************************
//*****************************************************************************
namespace xbridge
{

//*****************************************************************************
//*****************************************************************************
void badaboom()
{
    int * a = 0;
    *a = 0;
}

//*****************************************************************************
//*****************************************************************************
class App::Impl
{
    friend class App;

    enum
    {
        TIMER_INTERVAL = 60
    };

protected:
    /**
     * @brief Impl - constructor, init timer and threads
     */
    Impl();

    /**
     * @brief start
     * @return
     */
    bool start();
    /**
     * @brief stop
     * @return
     */
    bool stop();

protected:
    /**
     * @brief onSend
     * @param id
     * @param message
     */
    void onSend(const std::vector<unsigned char> & id, const std::vector<unsigned char> & message);

    /**
     * @brief onTimer
     */
    void onTimer();

    /**
     * @brief getSession
     * @return
     */
    SessionPtr getSession();
    /**
     * @brief getSession
     * @param address
     * @return
     */
    SessionPtr getSession(const std::vector<unsigned char> & address);

protected:
    // workers
    std::deque<IoServicePtr>                        m_services;
    std::deque<WorkPtr>                             m_works;
    boost::thread_group                             m_threads;

    // timer
    boost::asio::io_service                         m_timerIo;
    std::shared_ptr<boost::asio::io_service::work>  m_timerIoWork;
    boost::thread                                   m_timerThread;
    boost::asio::deadline_timer                     m_timer;

    // sessions
    mutable boost::mutex                            m_sessionsLock;
    SessionQueue                                    m_sessions;
    SessionsAddrMap                                 m_sessionAddressMap;

    // connectors
    mutable boost::mutex                            m_connectorsLock;
    Connectors                                      m_connectors;
    ConnectorsAddrMap                               m_connectorAddressMap;
    ConnectorsCurrencyMap                           m_connectorCurrencyMap;

    // pending messages (packet processing loop)
    boost::mutex                                    m_messagesLock;
    typedef std::set<uint256> ProcessedMessages;
    ProcessedMessages                               m_processedMessages;

    // address book
    boost::mutex                                    m_addressBookLock;
    AddressBook                                     m_addressBook;
    std::set<std::string>                           m_addresses;

    // transactions
    boost::mutex                                    m_txLocker;
    std::map<uint256, TransactionDescrPtr>          m_transactions;
    std::map<uint256, TransactionDescrPtr>          m_historicTransactions;

    // utxo records
    boost::mutex                                    m_utxoLocker;
    std::set<wallet::UtxoEntry>                     m_utxoItems;

    // network packets queue
    boost::mutex                                    m_ppLocker;
    std::map<uint256, XBridgePacketPtr>             m_pendingPackets;
};

//*****************************************************************************
//*****************************************************************************
App::Impl::Impl()
    : m_timerIoWork(new boost::asio::io_service::work(m_timerIo))
    , m_timerThread(boost::bind(&boost::asio::io_service::run, &m_timerIo))
    , m_timer(m_timerIo, boost::posix_time::seconds(TIMER_INTERVAL))
{

}

//*****************************************************************************
//*****************************************************************************
App::App()
    : m_p(new Impl)
{
}

//*****************************************************************************
//*****************************************************************************
App::~App()
{
    stop();

#ifdef WIN32
    WSACleanup();
#endif
}

//*****************************************************************************
//*****************************************************************************
// static
App & App::instance()
{
    static App app;
    return app;
}

//*****************************************************************************
//*****************************************************************************
// static
std::string App::version()
{
    std::ostringstream o;
    o << XBRIDGE_VERSION_MAJOR
      << "." << XBRIDGE_VERSION_MINOR
      << "." << XBRIDGE_VERSION_DESCR
      << " [" << XBRIDGE_VERSION << "]";
    return o.str();
}

//*****************************************************************************
//*****************************************************************************
// static
bool App::isEnabled()
{
    // enabled by default
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool App::start()
{
    return m_p->start();
}

//*****************************************************************************
//*****************************************************************************
bool App::Impl::start()
{
    // start xbrige
    try
    {
        // services and threas
        for (int i = 0; i < boost::thread::hardware_concurrency(); ++i)
        {
            IoServicePtr ios(new boost::asio::io_service);

            m_services.push_back(ios);
            m_works.push_back(WorkPtr(new boost::asio::io_service::work(*ios)));

            m_threads.create_thread(boost::bind(&boost::asio::io_service::run, ios));
        }

        m_timer.async_wait(boost::bind(&Impl::onTimer, this));

        // sessions
        xbridge::App & app = xbridge::App::instance();
        {
            Settings & s = settings();
            std::vector<std::string> wallets = s.exchangeWallets();
            for (std::vector<std::string>::iterator i = wallets.begin(); i != wallets.end(); ++i)
            {
                WalletParam wp;
                wp.currency                    = *i;
                wp.title                       = s.get<std::string>(*i + ".Title");
                wp.address                     = s.get<std::string>(*i + ".Address");
                wp.m_ip                          = s.get<std::string>(*i + ".Ip");
                wp.m_port                        = s.get<std::string>(*i + ".Port");
                wp.m_user                        = s.get<std::string>(*i + ".Username");
                wp.m_passwd                      = s.get<std::string>(*i + ".Password");
                wp.addrPrefix[0]               = s.get<int>(*i + ".AddressPrefix", 0);
                wp.scriptPrefix[0]             = s.get<int>(*i + ".ScriptPrefix", 0);
                wp.secretPrefix[0]             = s.get<int>(*i + ".SecretPrefix", 0);
                wp.COIN                        = s.get<uint64_t>(*i + ".COIN", 0);
                wp.txVersion                   = s.get<uint32_t>(*i + ".TxVersion", 1);
                wp.minTxFee                    = s.get<uint64_t>(*i + ".MinTxFee", 0);
                wp.feePerByte                  = s.get<uint64_t>(*i + ".FeePerByte", 200);
                wp.m_minAmount                 = s.get<uint64_t>(*i + ".MinimumAmount", 0);
                wp.dustAmount                  = 3 * wp.minTxFee;
                wp.method                      = s.get<std::string>(*i + ".CreateTxMethod");
                wp.isGetNewPubKeySupported     = s.get<bool>(*i + ".GetNewKeySupported", false);
                wp.isImportWithNoScanSupported = s.get<bool>(*i + ".ImportWithNoScanSupported", false);
                wp.blockTime                   = s.get<int>(*i + ".BlockTime", 0);
                wp.requiredConfirmations       = s.get<int>(*i + ".Confirmations", 0);

                if (wp.m_ip.empty() || wp.m_port.empty() ||
                    wp.m_user.empty() || wp.m_passwd.empty() ||
                    wp.COIN == 0 || wp.blockTime == 0)
                {
                    LOG() << "read wallet " << *i << " with empty parameters>";
                    continue;
                }
                else
                {
                    LOG() << "read wallet " << *i << " [" << wp.title << "] " << wp.m_ip
                          << ":" << wp.m_port; // << " COIN=" << wp.COIN;
                }

                xbridge::WalletConnectorPtr conn;
                if (wp.method == "ETHER")
                {
                    LOG() << "wp.method ETHER not implemented" << __FUNCTION__;
                    // session.reset(new XBridgeSessionEthereum(wp));
                }
                else if (wp.method == "BTC")
                {
                    conn.reset(new BtcWalletConnector);
                    *conn = wp;
                }
                else if (wp.method == "BCC")
                {
                    conn.reset(new BccWalletConnector);
                    *conn = wp;
                }
//                else if (wp.method == "RPC")
//                {
//                    LOG() << "wp.method RPC not implemented" << __FUNCTION__;
//                    // session.reset(new XBridgeSessionRpc(wp));
//                }
                else
                {
                    // session.reset(new XBridgeSession(wp));
                    ERR() << "unknown session type " << __FUNCTION__;
                }
                if (conn)
                {
                    app.addConnector(conn);
                }
            }
        }
    }
    catch (std::exception & e)
    {
        ERR() << e.what();
        ERR() << __FUNCTION__;
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool App::init(int argc, char *argv[])
{
    // init xbridge settings
    Settings & s = settings();
    {
        std::string path(GetDataDir(false).string());
        path += "/xbridge.conf";
        s.read(path.c_str());
        s.parseCmdLine(argc, argv);
        LOG() << "Finished loading config" << path;
    }

    // init secp256
    ECC_Start();

    // init exchange
    Exchange & e = Exchange::instance();
    e.init();

    // sessions
    {
        boost::mutex::scoped_lock l(m_p->m_sessionsLock);

        for (uint32_t i = 0; i < boost::thread::hardware_concurrency(); ++i)
        {
            SessionPtr ptr(new Session());
            m_p->m_sessions.push(ptr);
            m_p->m_sessionAddressMap[ptr->sessionAddr()] = ptr;
        }
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool App::stop()
{
    return m_p->stop();
}

//*****************************************************************************
//*****************************************************************************
bool App::Impl::stop()
{
    LOG() << "stopping threads...";

    m_timer.cancel();
    m_timerIo.stop();
    m_timerIoWork.reset();
    m_timerThread.join();

//    for (IoServicePtr & i : m_services)
//    {
//        i->stop();
//    }
    for (WorkPtr & i : m_works)
    {
        i.reset();
    }

    m_threads.join_all();

    // secp stop
    ECC_Stop();

    return true;
}

//*****************************************************************************
//*****************************************************************************
void App::sendPacket(const XBridgePacketPtr & packet)
{
    static std::vector<unsigned char> addr(20, 0);
    std::vector<unsigned char> v(packet->header(), packet->header()+packet->allSize());
    m_p->onSend(addr, v);
}

//*****************************************************************************
// send packet to xbridge network to specified id,
// or broadcast, when id is empty
//*****************************************************************************
void App::Impl::onSend(const std::vector<unsigned char> & id, const std::vector<unsigned char> & message)
{
    std::vector<unsigned char> msg(id);
    if (msg.size() != 20)
    {
        ERR() << "bad send address " << __FUNCTION__;
        return;
    }

    // timestamp
    uint64_t timestamp = std::time(0);
    unsigned char * ptr = reinterpret_cast<unsigned char *>(&timestamp);
    msg.insert(msg.end(), ptr, ptr + sizeof(uint64_t));

    // body
    msg.insert(msg.end(), message.begin(), message.end());

    uint256 hash = Hash(msg.begin(), msg.end());

    LOCK(cs_vNodes);
    for  (CNode * pnode : vNodes)
    {
        if (pnode->setKnown.insert(hash).second)
        {
            pnode->PushMessage("xbridge", msg);
        }
    }
}

//*****************************************************************************
//*****************************************************************************
void App::sendPacket(const std::vector<unsigned char> & id, const XBridgePacketPtr & packet)
{
    std::vector<unsigned char> v;
    std::copy(packet->header(), packet->header()+packet->allSize(), std::back_inserter(v));
    m_p->onSend(id, v);
}

//*****************************************************************************
//*****************************************************************************
SessionPtr App::Impl::getSession()
{
    SessionPtr ptr;

    boost::mutex::scoped_lock l(m_sessionsLock);
    ptr = m_sessions.front();
    m_sessions.pop();
    m_sessions.push(ptr);

    return ptr;
}

//*****************************************************************************
//*****************************************************************************
SessionPtr App::Impl::getSession(const std::vector<unsigned char> & address)
{
    boost::mutex::scoped_lock l(m_sessionsLock);
    if (m_sessionAddressMap.count(address))
    {
        return m_sessionAddressMap[address];
    }

    return SessionPtr();
}

//*****************************************************************************
//*****************************************************************************
void App::onMessageReceived(const std::vector<unsigned char> & id,
                                   const std::vector<unsigned char> & message,
                                   CValidationState & /*state*/)
{
    if (isKnownMessage(message))
    {
        return;
    }

    addToKnown(message);

    XBridgePacketPtr packet(new XBridgePacket);
    if (!packet->copyFrom(message))
    {
        LOG() << "incorrect packet received";
        return;
    }

    LOG() << "received message to " << util::base64_encode(std::string((char *)&id[0], 20)).c_str()
             << " command " << packet->command();

    if (!Session::checkXBridgePacketVersion(packet))
    {
        // ERR() << "incorrect protocol version <" << packet->version() << "> " << __FUNCTION__;
        return;
    }

    // check direct session address
    SessionPtr ptr = m_p->getSession(id);
    if (ptr)
    {
        ptr->processPacket(packet);
    }

    else
    {
        {
            // if no session address - find connector address
            boost::mutex::scoped_lock l(m_p->m_connectorsLock);
            if (m_p->m_connectorAddressMap.count(id))
            {
                ptr = m_p->getSession();
            }
        }

        if (ptr)
        {
            ptr->processPacket(packet);
        }
    }
}

//*****************************************************************************
//*****************************************************************************
void App::onBroadcastReceived(const std::vector<unsigned char> & message,
                                     CValidationState & /*state*/)
{
    if (isKnownMessage(message))
    {
        return;
    }

    addToKnown(message);

    // process message
    XBridgePacketPtr packet(new XBridgePacket);
    if (!packet->copyFrom(message))
    {
        LOG() << "incorrect broadcast packet received";
        return;
    }

    LOG() << "broadcast message, command " << packet->command();

    if (!Session::checkXBridgePacketVersion(packet))
    {
        // ERR() << "incorrect protocol version <" << packet->version() << "> " << __FUNCTION__;
        return;
    }

    SessionPtr ptr = m_p->getSession();
    if (ptr)
    {
        ptr->processPacket(packet);
    }
}

//*****************************************************************************
//*****************************************************************************
bool App::processLater(const uint256 & txid, const XBridgePacketPtr & packet)
{
    boost::mutex::scoped_lock l(m_p->m_ppLocker);
    m_p->m_pendingPackets[txid] = packet;
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool App::removePackets(const uint256 & txid)
{
    // remove from pending packets (if added)

    boost::mutex::scoped_lock l(m_p->m_ppLocker);
    size_t removed = m_p->m_pendingPackets.erase(txid);
    assert(removed < 2 && "duplicate packets in packets queue");

    return true;
}

//*****************************************************************************
//*****************************************************************************
WalletConnectorPtr App::connectorByCurrency(const std::string & currency) const
{
    boost::mutex::scoped_lock l(m_p->m_connectorsLock);
    if (m_p->m_connectorCurrencyMap.count(currency))
    {
        return m_p->m_connectorCurrencyMap.at(currency);
    }

    return WalletConnectorPtr();
}

//*****************************************************************************
//*****************************************************************************
std::vector<std::string> App::availableCurrencies() const
{
    boost::mutex::scoped_lock l(m_p->m_connectorsLock);

    std::vector<std::string> currencies;

    for(auto i = m_p->m_connectorCurrencyMap.begin(); i != m_p->m_connectorCurrencyMap.end();)
    {
        currencies.push_back(i->first);
        ++i;
    }

    return currencies;
}

//*****************************************************************************
//*****************************************************************************
bool App::hasCurrency(const std::string & currency) const
{
    boost::mutex::scoped_lock l(m_p->m_connectorsLock);
    return m_p->m_connectorCurrencyMap.count(currency);
}

//*****************************************************************************
//*****************************************************************************
void App::addConnector(const WalletConnectorPtr & conn)
{
    boost::mutex::scoped_lock l(m_p->m_connectorsLock);
    m_p->m_connectors.push_back(conn);
    m_p->m_connectorCurrencyMap[conn->currency] = conn;
}

//*****************************************************************************
//*****************************************************************************
void App::updateConnector(const WalletConnectorPtr & conn,
                          const std::vector<unsigned char> addr,
                          const std::string & currency)
{
    boost::mutex::scoped_lock l(m_p->m_connectorsLock);

    m_p->m_connectorAddressMap[addr]      = conn;
    m_p->m_connectorCurrencyMap[currency] = conn;
}

//*****************************************************************************
//*****************************************************************************
std::vector<WalletConnectorPtr> App::connectors() const
{
    boost::mutex::scoped_lock l(m_p->m_connectorsLock);
    return m_p->m_connectors;
}

//*****************************************************************************
//*****************************************************************************
bool App::isKnownMessage(const std::vector<unsigned char> & message)
{
    boost::mutex::scoped_lock l(m_p->m_messagesLock);
    return m_p->m_processedMessages.count(Hash(message.begin(), message.end())) > 0;
}

//*****************************************************************************
//*****************************************************************************
void App::addToKnown(const std::vector<unsigned char> & message)
{
    // add to known
    boost::mutex::scoped_lock l(m_p->m_messagesLock);
    m_p->m_processedMessages.insert(Hash(message.begin(), message.end()));
}

//*****************************************************************************
//*****************************************************************************
bool App::checkUtxoItems(const std::vector<wallet::UtxoEntry> & items)
{
    for (const wallet::UtxoEntry & item : items)
    {
        if (txOutIsLocked(item))
        {
            return false;
        }
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool App::lockUtxoItems(const std::vector<wallet::UtxoEntry> & items)
{
    bool hasDuplicate = false;
    boost::mutex::scoped_lock l(m_p->m_utxoLocker);
    for (const wallet::UtxoEntry & item : items)
    {
        if (!m_p->m_utxoItems.insert(item).second)
        {
            // duplicate items
            hasDuplicate = true;
            break;
        }
    }

    if (hasDuplicate)
    {
        // TODO remove items?
        return false;
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool App::txOutIsLocked(const wallet::UtxoEntry & entry) const
{
    boost::mutex::scoped_lock l(m_p->m_utxoLocker);
    if (m_p->m_utxoItems.count(entry))
    {
        return true;
    }
    return false;
}

//******************************************************************************
//******************************************************************************
TransactionDescrPtr App::transaction(const uint256 & id) const
{
    TransactionDescrPtr result;

    boost::mutex::scoped_lock l(m_p->m_txLocker);

    if (m_p->m_transactions.count(id))
    {
        result = m_p->m_transactions[id];
    }

    if (m_p->m_historicTransactions.count(id))
    {
        assert(!result && "duplicate objects");
        result = m_p->m_historicTransactions[id];
    }

    return result;
}

//******************************************************************************
//******************************************************************************
std::map<uint256, xbridge::TransactionDescrPtr> App::transactions() const
{
    boost::mutex::scoped_lock l(m_p->m_txLocker);
    return m_p->m_transactions;
}

//******************************************************************************
//******************************************************************************
std::map<uint256, xbridge::TransactionDescrPtr> App::history() const
{
    boost::mutex::scoped_lock l(m_p->m_txLocker);
    return m_p->m_historicTransactions;
}

//******************************************************************************
//******************************************************************************
void App::appendTransaction(const TransactionDescrPtr & ptr)
{
    boost::mutex::scoped_lock l(m_p->m_txLocker);

    if (m_p->m_historicTransactions.count(ptr->id))
    {
        return;
    }

    if (!m_p->m_transactions.count(ptr->id))
    {
        // new transaction, copy data
        m_p->m_transactions[ptr->id] = ptr;
    }
    else
    {
        // existing, update timestamp
        m_p->m_transactions[ptr->id]->updateTimestamp(*ptr);
    }
}

//******************************************************************************
//******************************************************************************
void App::moveTransactionToHistory(const uint256 & id)
{
    TransactionDescrPtr xtx;

    {
        boost::mutex::scoped_lock l(m_p->m_txLocker);

        size_t counter = 0;

        if (m_p->m_transactions.count(id))
        {
            xtx = m_p->m_transactions[id];

            counter = m_p->m_transactions.erase(id);
            assert(counter < 2 && "duplicate transaction");
        }

        if (xtx)
        {
            assert(m_p->m_historicTransactions.count(id) == 0 && "duplicate tx in tx list and history");
            m_p->m_historicTransactions[id] = xtx;
        }
    }

    if (xtx)
    {
        // unlock tx coins
        WalletConnectorPtr conn = connectorByCurrency(xtx->fromCurrency);
        if (conn)
        {
            conn->lockUnspent(xtx->usedCoins, false);
        }
    }

    // remove pending packets for this tx
    removePackets(id);
}

//******************************************************************************
//******************************************************************************
xbridge::Error App::sendXBridgeTransaction(const std::string & from,
                                       const std::string & fromCurrency,
                                       const uint64_t & fromAmount,
                                       const std::string & to,
                                       const std::string & toCurrency,
                                       const uint64_t & toAmount,
                                       uint256 & id)
{
    if (fromCurrency.size() > 8 || toCurrency.size() > 8)
    {
        WARN() << "invalid currency " << __FUNCTION__;
        return xbridge::Error::INVALID_CURRENCY;
    }

    WalletConnectorPtr connFrom = connectorByCurrency(fromCurrency);
    WalletConnectorPtr connTo   = connectorByCurrency(toCurrency);
    if (!connFrom || !connTo)
    {
        // no session
        WARN() << "no session for <" << fromCurrency << "> " << __FUNCTION__;
        return xbridge::Error::NO_SESSION;
    }

    // check amount
    std::vector<wallet::UtxoEntry> outputs;
    connFrom->getUnspent(outputs);

    double utxoAmount = 0;
    std::vector<wallet::UtxoEntry> outputsForUse;
    for (const wallet::UtxoEntry & entry : outputs)
    {
        if (!txOutIsLocked(entry))
        {
            utxoAmount += entry.amount;
            outputsForUse.push_back(entry);

            // TODO calculate fee for outputsForUse.count()

            if (utxoAmount > fromAmount)
            {
                break;
            }
        }
    }

    if ((utxoAmount * TransactionDescr::COIN) < fromAmount)
    {
        WARN() << "insufficient funds for <" << fromCurrency << "> " << __FUNCTION__;
        return xbridge::Error::INSIFFICIENT_FUNDS;
    }

    boost::uint32_t timestamp = time(0);
    id = Hash(from.begin(), from.end(),
              fromCurrency.begin(), fromCurrency.end(),
              BEGIN(fromAmount), END(fromAmount),
              to.begin(), to.end(),
              toCurrency.begin(), toCurrency.end(),
              BEGIN(toAmount), END(toAmount),
              BEGIN(timestamp), END(timestamp));

    TransactionDescrPtr ptr(new TransactionDescr);
    ptr->id           = id;
    ptr->from         = connFrom->toXAddr(from);
    ptr->fromCurrency = fromCurrency;
    ptr->fromAmount   = fromAmount;
    ptr->to           = connTo->toXAddr(to);
    ptr->toCurrency   = toCurrency;
    ptr->toAmount     = toAmount;
    ptr->usedCoins    = outputsForUse;

    // try send immediatelly
    sendPendingTransaction(ptr);

//    LOG() << "accept transaction " << util::to_str(ptr->id) << std::endl
//          << "    from " << from << " (" << util::to_str(ptr->from) << ")" << std::endl
//          << "             " << ptr->fromCurrency << " : " << ptr->fromAmount << std::endl
//          << "    from " << to << " (" << util::to_str(ptr->to) << ")" << std::endl
//          << "             " << ptr->toCurrency << " : " << ptr->toAmount << std::endl;

    // lock used coins
    // TODO temporary disabled
    // connFrom->lockUnspent(ptr->usedCoins, true);

    {
        boost::mutex::scoped_lock l(m_p->m_txLocker);
        m_p->m_transactions[id] = ptr;
    }

    return xbridge::Error::SUCCESS;
}

//******************************************************************************
//******************************************************************************
bool App::sendPendingTransaction(const TransactionDescrPtr & ptr)
{
    // if (!ptr->packet)
    {
        if (ptr->from.size() == 0 || ptr->to.size() == 0)
        {
            // TODO temporary
            return false;
        }

        if (ptr->packet && ptr->packet->command() != xbcTransaction)
        {
            // not send pending packets if not an xbcTransaction
            return false;
        }

        ptr->packet.reset(new XBridgePacket(xbcTransaction));

        // field length must be 8 bytes
        std::vector<unsigned char> fc(8, 0);
        std::copy(ptr->fromCurrency.begin(), ptr->fromCurrency.end(), fc.begin());

        // field length must be 8 bytes
        std::vector<unsigned char> tc(8, 0);
        std::copy(ptr->toCurrency.begin(), ptr->toCurrency.end(), tc.begin());

        // 32 bytes - id of transaction
        // 2x
        // 20 bytes - address
        //  8 bytes - currency
        //  8 bytes - amount
        ptr->packet->append(ptr->id.begin(), 32);
        ptr->packet->append(ptr->from);
        ptr->packet->append(fc);
        ptr->packet->append(ptr->fromAmount);
        ptr->packet->append(ptr->to);
        ptr->packet->append(tc);
        ptr->packet->append(ptr->toAmount);

        // utxo items
        ptr->packet->append(static_cast<uint32_t>(ptr->usedCoins.size()));
        for (const wallet::UtxoEntry & entry : ptr->usedCoins)
        {
            uint256 txid(entry.txId);
            ptr->packet->append(txid.begin(), 32);
            ptr->packet->append(entry.vout);
        }
    }

    sendPacket(ptr->packet);

    ptr->state = TransactionDescr::trPending;
    xuiConnector.NotifyXBridgeTransactionStateChanged(ptr->id);

    return true;
}

//******************************************************************************
//******************************************************************************
Error App::acceptXBridgeTransaction(const uint256     & id,
                                    const std::string & from,
                                    const std::string & to)
{
    TransactionDescrPtr ptr;

    {
        boost::mutex::scoped_lock l(m_p->m_txLocker);
        if (!m_p->m_transactions.count(id))
        {

            WARN() << "transaction not found " << __FUNCTION__;
            return xbridge::TRANSACTION_NOT_FOUND;
        }
        ptr = m_p->m_transactions[id];
    }

    WalletConnectorPtr connFrom = connectorByCurrency(ptr->fromCurrency);
    WalletConnectorPtr connTo   = connectorByCurrency(ptr->toCurrency);
    if (!connFrom || !connTo)
    {
        // no session
        WARN() << "no session for <" << ptr->toCurrency << "> " << __FUNCTION__;
        return xbridge::NO_SESSION;
    }

    // check amount
    std::vector<wallet::UtxoEntry> outputs;
    connFrom->getUnspent(outputs);

    double utxoAmount = 0;
    std::vector<wallet::UtxoEntry> outputsForUse;
    for (const wallet::UtxoEntry & entry : outputs)
    {
        if (!txOutIsLocked(entry))
        {
            utxoAmount += entry.amount;
            outputsForUse.push_back(entry);

            // TODO calculate fee for outputsForUse.count()

            if (utxoAmount > ptr->fromAmount)
            {
                break;
            }
        }
    }

    if ((utxoAmount * TransactionDescr::COIN) < ptr->fromAmount)
    {
        WARN() << "insufficient funds for <" << ptr->fromCurrency << "> " << __FUNCTION__;
        return xbridge::INSIFFICIENT_FUNDS;
    }

    ptr->from = connFrom->toXAddr(from);
    ptr->to   = connTo->toXAddr(to);
    // std::swap(ptr->fromCurrency, ptr->toCurrency);
    // std::swap(ptr->fromAmount,   ptr->toAmount);
    ptr->usedCoins = outputsForUse;

    // try send immediatelly
    sendAcceptingTransaction(ptr);

//    LOG() << "accept transaction " << util::to_str(ptr->id) << std::endl
//          << "    from " << from << " (" << util::to_str(ptr->from) << ")" << std::endl
//          << "             " << ptr->fromCurrency << " : " << ptr->fromAmount << std::endl
//          << "    from " << to << " (" << util::to_str(ptr->to) << ")" << std::endl
//          << "             " << ptr->toCurrency << " : " << ptr->toAmount << std::endl;


    // lock used coins
    // TODO temporary disabled
    // connTo->lockUnspent(ptr->usedCoins, true);

    return xbridge::Error::SUCCESS;
}

//******************************************************************************
//******************************************************************************
bool App::sendAcceptingTransaction(const TransactionDescrPtr & ptr)
{
    ptr->packet.reset(new XBridgePacket(xbcTransactionAccepting));

    // field length must be 8 bytes
    std::vector<unsigned char> fc(8, 0);
    std::copy(ptr->fromCurrency.begin(), ptr->fromCurrency.end(), fc.begin());

    // field length must be 8 bytes
    std::vector<unsigned char> tc(8, 0);
    std::copy(ptr->toCurrency.begin(), ptr->toCurrency.end(), tc.begin());

    // 20 bytes - id of transaction
    // 2x
    // 20 bytes - address
    //  8 bytes - currency
    //  4 bytes - amount
    ptr->packet->append(ptr->hubAddress);
    ptr->packet->append(ptr->id.begin(), 32);
    ptr->packet->append(ptr->from);
    ptr->packet->append(fc);
    ptr->packet->append(ptr->fromAmount);
    ptr->packet->append(ptr->to);
    ptr->packet->append(tc);
    ptr->packet->append(ptr->toAmount);

    // utxo items
    ptr->packet->append(static_cast<uint32_t>(ptr->usedCoins.size()));
    for (const wallet::UtxoEntry & entry : ptr->usedCoins)
    {
        uint256 txid(entry.txId);
        ptr->packet->append(txid.begin(), 32);
        ptr->packet->append(entry.vout);
    }

    sendPacket(ptr->hubAddress, ptr->packet);

    ptr->state = TransactionDescr::trAccepting;
    xuiConnector.NotifyXBridgeTransactionStateChanged(ptr->id);

    return true;
}

//******************************************************************************
//******************************************************************************
xbridge::Error App::cancelXBridgeTransaction(const uint256 &id,
                                             const TxCancelReason &reason)
{
    if (sendCancelTransaction(id, reason))
    {
        TransactionDescrPtr xtx = transaction(id);
        xtx->state  = TransactionDescr::trCancelled;
        xtx->reason = reason;

        xuiConnector.NotifyXBridgeTransactionStateChanged(id);

        moveTransactionToHistory(id);
    }

    return xbridge::SUCCESS;
}

//******************************************************************************
//******************************************************************************
xbridge::Error App::rollbackXBridgeTransaction(const uint256 & id)
{
    WalletConnectorPtr conn;
    {
        boost::mutex::scoped_lock l(m_p->m_txLocker);

        if (m_p->m_transactions.count(id))
        {
            TransactionDescrPtr ptr = m_p->m_transactions[id];
            if (!ptr->refTx.empty())
            {
                conn = connectorByCurrency(ptr->fromCurrency);
                if (!conn)
                {
                    ERR() << "unknown session for currency " + ptr->fromCurrency << __FUNCTION__;
                    return xbridge::UNKNOWN_SESSION;
                }
            }
        }
    }

//    if (conn)
//    {
//        // session use m_txLocker, must be unlocked because not recursive
//        if (!conn->rollbacktXBridgeTransaction(id))
//        {
//            LOG() << "revert tx failed for " << id.ToString();
//            return false;
//        }
//
//        sendRollbackTransaction(id);
//    }
    return xbridge::SUCCESS;
}

//******************************************************************************
//******************************************************************************
bool App::sendCancelTransaction(const uint256 & txid,
                                       const TxCancelReason & reason)
{
    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionCancel));
    reply->append(txid.begin(), 32);
    reply->append(static_cast<uint32_t>(reason));

    static std::vector<unsigned char> addr(20, 0);
    sendPacket(addr, reply);

    // cancelled
    return true;
}

//******************************************************************************
//******************************************************************************
bool App::sendRollbackTransaction(const uint256 & txid)
{
    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionRollback));
    reply->append(txid.begin(), 32);

    static std::vector<unsigned char> addr(20, 0);
    sendPacket(addr, reply);

    // rolled back
    return true;
}

//******************************************************************************

//******************************************************************************
void App::Impl::onTimer()
{
    // DEBUG_TRACE();
    {
        m_services.push_back(m_services.front());
        m_services.pop_front();

        xbridge::SessionPtr session = getSession();

        IoServicePtr io = m_services.front();

        // call check expired transactions
        io->post(boost::bind(&xbridge::Session::checkFinishedTransactions, session));

        // send transactions list
        io->post(boost::bind(&xbridge::Session::sendListOfTransactions, session));

        // erase expired tx
        io->post(boost::bind(&xbridge::Session::eraseExpiredPendingTransactions, session));

        // get addressbook
        io->post(boost::bind(&xbridge::Session::getAddressBook, session));

        // unprocessed packets
        {
            std::map<uint256, XBridgePacketPtr> map;
            {
                boost::mutex::scoped_lock l(m_ppLocker);
                map = m_pendingPackets;
                m_pendingPackets.clear();
            }
            for (const std::pair<uint256, XBridgePacketPtr> & item : map)
            {

                xbridge::SessionPtr s = getSession();
                XBridgePacketPtr packet   = item.second;
                io->post(boost::bind(&xbridge::Session::processPacket, s, packet));

            }
        }
    }

    m_timer.expires_at(m_timer.expires_at() + boost::posix_time::seconds(TIMER_INTERVAL));
    m_timer.async_wait(boost::bind(&Impl::onTimer, this));
}

} // namespace xbridge

