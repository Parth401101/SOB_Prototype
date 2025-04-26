#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/csma-module.h"
#include "ns3/mobility-module.h"
#include <unordered_set>
#include <unordered_map>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <functional>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpGossip");

// Hash function for messages to reduce memory usage
class MessageHasher {
public:
    static uint64_t HashString(const std::string& msg) {
        std::hash<std::string> hasher;
        return hasher(msg);
    }
};

class TcpGossipApp : public Application {
private:
    Ptr<Socket> m_socket;
    std::vector<Ipv6Address> m_neighbors;

    // Connection pool implementation
    class ConnectionPool {
    private:
        // Only create a fixed number of outgoing connections at a time
        static const uint32_t MAX_ACTIVE_CONNECTIONS = 10;
        
        // Map to track persistent connections to neighbors
        std::map<Ipv6Address, Ptr<Socket>> m_neighborSockets;
        // Track if a socket is considered active
        std::map<Ipv6Address, bool> m_socketActive;
        // Track connection priority (lower = higher priority)
        std::map<Ipv6Address, uint32_t> m_connectionPriority;
        // Weak references to incoming sockets
        std::unordered_set<Ptr<Socket>> m_incomingSockets;
        
        // Last time we exchanged data with this neighbor
        std::map<Ipv6Address, double> m_lastActivity;
        
        // Reference to parent app
        TcpGossipApp* m_app;
        
    public:
        ConnectionPool(TcpGossipApp* app) : m_app(app) {}
        
        void AddNeighbor(Ipv6Address neighbor) {
            if (m_neighborSockets.find(neighbor) == m_neighborSockets.end()) {
                m_neighborSockets[neighbor] = nullptr;
                m_socketActive[neighbor] = false;
                m_connectionPriority[neighbor] = rand() % 100; // Random initial priority
                m_lastActivity[neighbor] = 0.0;
            }
        }
        
        void RemoveNeighbor(Ipv6Address neighbor) {
            auto socketIt = m_neighborSockets.find(neighbor);
            if (socketIt != m_neighborSockets.end()) {
                if (socketIt->second) {
                    socketIt->second->Close();
                }
                m_neighborSockets.erase(socketIt);
            }
            m_socketActive.erase(neighbor);
            m_connectionPriority.erase(neighbor);
            m_lastActivity.erase(neighbor);
        }
        
        void AddIncomingSocket(Ptr<Socket> socket) {
            m_incomingSockets.insert(socket);
        }
        
        void RemoveIncomingSocket(Ptr<Socket> socket) {
            m_incomingSockets.erase(socket);
        }
        
        void UpdateActivity(Ipv6Address neighbor) {
            // Update last activity time
            m_lastActivity[neighbor] = Simulator::Now().GetSeconds();
            
            // Increase priority (lower number = higher priority)
            m_connectionPriority[neighbor] = m_connectionPriority[neighbor] / 2;
        }
        
        void UpdateActivityFromSocket(Ptr<Socket> socket) {
            // Find which neighbor this socket belongs to
            for (auto& pair : m_neighborSockets) {
                if (pair.second == socket) {
                    UpdateActivity(pair.first);
                    break;
                }
            }
        }
        
        void SetSocket(Ipv6Address neighbor, Ptr<Socket> socket) {
            m_neighborSockets[neighbor] = socket;
        }
        
        void SetSocketActive(Ipv6Address neighbor, bool active) {
            m_socketActive[neighbor] = active;
        }
        
        bool IsActive(Ipv6Address neighbor) const {
            auto it = m_socketActive.find(neighbor);
            return (it != m_socketActive.end() && it->second);
        }
        
        Ptr<Socket> GetSocket(Ipv6Address neighbor) {
            auto it = m_neighborSockets.find(neighbor);
            return (it != m_neighborSockets.end()) ? it->second : nullptr;
        }
        
        uint32_t GetActiveConnectionCount() const {
            uint32_t count = 0;
            for (const auto& pair : m_socketActive) {
                if (pair.second) count++;
            }
            return count;
        }
        
        // Get neighbors ordered by priority
        std::vector<Ipv6Address> GetPriorityNeighbors() {
            std::vector<std::pair<Ipv6Address, uint32_t>> neighbors;
            
            for (const auto& pair : m_connectionPriority) {
                neighbors.push_back(std::make_pair(pair.first, pair.second));
            }
            
            // Sort by priority (lower number = higher priority)
            std::sort(neighbors.begin(), neighbors.end(), 
                    [](const auto& a, const auto& b) {
                        return a.second < b.second;
                    });
            
            std::vector<Ipv6Address> result;
            for (const auto& pair : neighbors) {
                result.push_back(pair.first);
            }
            
            return result;
        }
        
        void ManageConnections() {
            // Check if we need to establish more connections
            uint32_t activeConnections = GetActiveConnectionCount();
            
            if (activeConnections < MAX_ACTIVE_CONNECTIONS && m_neighborSockets.size() > 0) {
                // Get neighbors sorted by priority
                std::vector<Ipv6Address> priorityNeighbors = GetPriorityNeighbors();
                
                // Try to establish connections to high-priority neighbors first
                for (const auto& neighbor : priorityNeighbors) {
                    if (activeConnections >= MAX_ACTIVE_CONNECTIONS) break;
                    
                    if (!IsActive(neighbor)) {
                        // Schedule connection with a small delay
                        Simulator::Schedule(MilliSeconds(rand() % 100), 
                                          &TcpGossipApp::ConnectToNeighbor, 
                                          m_app, neighbor);
                        
                        activeConnections++;
                    }
                }
            }
            
            // If we have too many connections, close the least important ones
            if (activeConnections > MAX_ACTIVE_CONNECTIONS) {
                // Get neighbors in reverse priority order
                std::vector<Ipv6Address> priorityNeighbors = GetPriorityNeighbors();
                std::reverse(priorityNeighbors.begin(), priorityNeighbors.end());
                
                for (const auto& neighbor : priorityNeighbors) {
                    if (activeConnections <= MAX_ACTIVE_CONNECTIONS) break;
                    
                    if (IsActive(neighbor)) {
                        auto socket = GetSocket(neighbor);
                        if (socket) {
                            socket->Close();
                            SetSocket(neighbor, nullptr);
                        }
                        SetSocketActive(neighbor, false);
                        activeConnections--;
                    }
                }
            }
        }
        
        void CloseAllConnections() {
            for (auto& socketPair : m_neighborSockets) {
                if (socketPair.second) {
                    socketPair.second->Close();
                }
            }
            m_neighborSockets.clear();
            m_socketActive.clear();
            
            for (auto& socket : m_incomingSockets) {
                socket->Close();
            }
            m_incomingSockets.clear();
        }
    };
    
    // Message manager with bloom filter to reduce memory usage
    class MessageManager {
    private:
        // Simple bloom filter implementation
        class BloomFilter {
        private:
            std::vector<bool> m_bits;
            size_t m_size;
            uint32_t m_numHashes;
            
            // Hash function
            size_t hash(const uint64_t key, uint32_t seed) const {
                uint64_t h = key + seed;
                h ^= h >> 33;
                h *= 0xff51afd7ed558ccdL;
                h ^= h >> 33;
                h *= 0xc4ceb9fe1a85ec53L;
                h ^= h >> 33;
                return h % m_size;
            }
            
        public:
            BloomFilter(size_t size = 10000, uint32_t numHashes = 5) 
                : m_size(size), m_numHashes(numHashes) {
                m_bits.resize(size, false);
            }
            
            void insert(uint64_t key) {
                for (uint32_t i = 0; i < m_numHashes; i++) {
                    size_t pos = hash(key, i);
                    m_bits[pos] = true;
                }
            }
            
            bool contains(uint64_t key) const {
                for (uint32_t i = 0; i < m_numHashes; i++) {
                    size_t pos = hash(key, i);
                    if (!m_bits[pos]) return false;
                }
                return true;
            }
        };
        
        BloomFilter m_receivedFilter;     // Bloom filter for received messages
        BloomFilter m_forwardedFilter;    // Bloom filter for forwarded messages
        
        // Backup exact tracking for important messages (blocks)
        std::unordered_set<uint64_t> m_receivedBlocks;
        uint32_t m_receivedBlockCount;
        
    public:
        MessageManager() : m_receivedBlockCount(0) {
            // Use larger bloom filters for large networks
            m_receivedFilter = BloomFilter(100000, 5);
            m_forwardedFilter = BloomFilter(100000, 5);
        }
        
        bool IsReceived(const std::string& msg) const {
            uint64_t hash = MessageHasher::HashString(msg);
            return m_receivedFilter.contains(hash);
        }
        
        bool IsForwarded(const std::string& msg) const {
            uint64_t hash = MessageHasher::HashString(msg);
            return m_forwardedFilter.contains(hash);
        }
        
        void MarkReceived(const std::string& msg) {
            uint64_t hash = MessageHasher::HashString(msg);
            m_receivedFilter.insert(hash);
            
            // Track blocks separately
            if (msg.find("Block_") == 0) {
                m_receivedBlocks.insert(hash);
                m_receivedBlockCount++;
            }
        }
        
        void MarkForwarded(const std::string& msg) {
            uint64_t hash = MessageHasher::HashString(msg);
            m_forwardedFilter.insert(hash);
        }
        
        uint32_t GetReceivedBlockCount() const {
            return m_receivedBlockCount;
        }
    };
    
    ConnectionPool m_connectionPool;
    MessageManager m_messageManager;
    
    Ipv6Address m_myAddress;
    uint32_t m_nodeId;
    bool m_isSender;
    
    // For connection management
    EventId m_connectionCheckEvent;
    bool m_connectionsEstablished;
    
    // For batched message forwarding
    EventId m_forwardEvent;
    std::vector<std::string> m_pendingMessages;
    static const uint32_t MAX_PENDING_MESSAGES = 20;
    static const Time FORWARD_INTERVAL;

public:
    TcpGossipApp(Ipv6Address myAddress) 
        : m_myAddress(myAddress), 
          m_isSender(false),
          m_connectionsEstablished(false),
          m_connectionPool(this),
          m_messageManager() {}

    void AddNeighbor(Ipv6Address neighbor) {
        if (neighbor != m_myAddress) {
            m_neighbors.push_back(neighbor);
            m_connectionPool.AddNeighbor(neighbor);
        }
    }

    void GetNeighbors(std::vector<Ipv6Address>& neighbors) const {
        neighbors = m_neighbors;
    }

    void RemoveNeighbor(Ipv6Address neighbor) {
        for (auto it = m_neighbors.begin(); it != m_neighbors.end(); ++it) {
            if (*it == neighbor) {
                m_neighbors.erase(it);
                m_connectionPool.RemoveNeighbor(neighbor);
                return;
            }
        }
    }

    void PrintNeighbors() const {
        std::cout << "Neighbors of " << m_myAddress << " (Node " << m_nodeId << "):" << std::endl;
        for (const auto& neighbor : m_neighbors) {
            std::cout << "  " << neighbor << std::endl;
        }
    }
    
    void StartApplication() override {
        m_nodeId = GetNode()->GetId();
        
        // Only print neighbors for a small subset of nodes
        if (m_nodeId % 50 == 0) {
            PrintNeighbors();
        }
        
        // Create listening socket
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_socket->Bind(Inet6SocketAddress(Ipv6Address::GetAny(), 8080));
        m_socket->Listen();

        m_socket->SetAcceptCallback(
            MakeCallback(&TcpGossipApp::AcceptConnection, this),
            MakeCallback(&TcpGossipApp::HandleAccept, this)
        );
        
        // Schedule connection establishment with delay based on node ID
        // This staggers connection attempts to prevent network flood
        Time delay = MilliSeconds(100 + (m_nodeId % 1000));
        Simulator::Schedule(delay, &TcpGossipApp::EstablishConnections, this);
        
        // Schedule periodic connection check with staggered timing
        m_connectionCheckEvent = Simulator::Schedule(
            Seconds(2.0 + (double)(m_nodeId % 100) / 100.0), 
            &TcpGossipApp::CheckConnections, this);
    }
    
    void StopApplication() override {
        // Cancel scheduled events
        if (m_connectionCheckEvent.IsRunning()) {
            Simulator::Cancel(m_connectionCheckEvent);
        }
        
        if (m_forwardEvent.IsRunning()) {
            Simulator::Cancel(m_forwardEvent);
        }
        
        // Close all connections
        m_connectionPool.CloseAllConnections();
        
        // Close listening socket
        if (m_socket) {
            m_socket->Close();
            m_socket = nullptr;
        }
    }
    
    void EstablishConnections() {
        // Let the connection pool manage connections
        m_connectionPool.ManageConnections();
        m_connectionsEstablished = true;
    }
    
    void ConnectToNeighbor(Ipv6Address neighborAddr) {
        // Create a new socket for this neighbor
        Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        
        socket->SetConnectCallback(
            MakeCallback(&TcpGossipApp::ConnectionSucceeded, this),
            MakeCallback(&TcpGossipApp::ConnectionFailed, this)
        );
        
        socket->SetRecvCallback(MakeCallback(&TcpGossipApp::ReceiveMessage, this));
        
        // Add this socket to our tracking
        m_connectionPool.SetSocket(neighborAddr, socket);
        m_connectionPool.SetSocketActive(neighborAddr, false); // Not active yet
        
        // Try to connect
        socket->Connect(Inet6SocketAddress(neighborAddr, 8080));
    }
    
    void CheckConnections() {
        // Manage connections periodically
        m_connectionPool.ManageConnections();
        
        // Schedule next check with a random variation to prevent synchronization
        double jitter = (double)(rand() % 500) / 1000.0; // 0-0.5s jitter
        m_connectionCheckEvent = Simulator::Schedule(
            Seconds(5.0 + jitter), 
            &TcpGossipApp::CheckConnections, this);
    }
    
    void SendHeartbeat(Ptr<Socket> socket) {
        // Send a compact heartbeat message
        std::string heartbeat = "h\n";
        Ptr<Packet> packet = Create<Packet>((uint8_t*)heartbeat.c_str(), heartbeat.size());
        socket->Send(packet);
    }

    bool AcceptConnection(Ptr<Socket> socket, const Address &from) {
        return true;  
    }

    void HandleAccept(Ptr<Socket> socket, const Address &from) {
        // Track this incoming socket
        m_connectionPool.AddIncomingSocket(socket);
        
        // Set up receive callback
        socket->SetRecvCallback(MakeCallback(&TcpGossipApp::ReceiveMessage, this));
    }

    void SendMessage(std::string msg) {
        if (m_messageManager.IsReceived(msg)) return;

        m_messageManager.MarkReceived(msg);
        AddToPendingMessages(msg);
    }

    void ReceiveMessage(Ptr<Socket> socket) {
        Address from;
        socket->GetPeerName(from);
        
        Ptr<Packet> packet = socket->Recv();
        if (!packet || packet->GetSize() == 0) {
            // Connection was closed or error
            m_connectionPool.RemoveIncomingSocket(socket);
            
            // Try to find which neighbor this was
            if (from.IsInvalid() == false) {
                try {
                    Inet6SocketAddress inet6Addr = Inet6SocketAddress::ConvertFrom(from);
                    Ipv6Address peerAddr = inet6Addr.GetIpv6();
                    m_connectionPool.SetSocketActive(peerAddr, false);
                } catch (const std::exception& e) {
                    // Address conversion failed, ignore
                }
            }
            
            return;
        }
        
        // Update activity on this connection
        if (from.IsInvalid() == false) {
            try {
                Inet6SocketAddress inet6Addr = Inet6SocketAddress::ConvertFrom(from);
                Ipv6Address peerAddr = inet6Addr.GetIpv6();
                m_connectionPool.UpdateActivity(peerAddr);
                m_connectionPool.SetSocketActive(peerAddr, true);
            } catch (const std::exception& e) {
                // Address conversion failed, ignore
            }
        } else {
            // Update based on the socket directly
            m_connectionPool.UpdateActivityFromSocket(socket);
        }
    
        // Process the packet
        uint32_t size = packet->GetSize();
        std::vector<uint8_t> buffer(size);  
        packet->CopyData(buffer.data(), size);
        
        // Process potentially multiple messages in the buffer
        std::string data(buffer.begin(), buffer.end());
        std::istringstream stream(data);
        std::string line;
        
        // Parse each line as a separate message
        while (std::getline(stream, line)) {
            // Remove any carriage returns that might be present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            // Skip empty lines
            if (line.empty()) continue;
            
            // Ignore heartbeat messages
            if (line == "h") {
                continue;
            }
            
            // Process the message if not received before
            if (!m_messageManager.IsReceived(line)) {
                m_messageManager.MarkReceived(line);
                
                // Add to pending messages with a random delay
                AddToPendingMessages(line);
            }
        }
    }
    
    void AddToPendingMessages(const std::string& msg) {
        m_pendingMessages.push_back(msg);
        
        // If this is the first pending message, schedule forwarding
        if (m_pendingMessages.size() == 1 && !m_forwardEvent.IsRunning()) {
            // Schedule with random delay to prevent network congestion
            Time delay = MilliSeconds(20 + (rand() % 200));
            m_forwardEvent = Simulator::Schedule(delay, &TcpGossipApp::ForwardPendingMessages, this);
        }
        // If we've reached the batch size, forward immediately
        else if (m_pendingMessages.size() >= MAX_PENDING_MESSAGES && !m_forwardEvent.IsRunning()) {
            m_forwardEvent = Simulator::Schedule(FORWARD_INTERVAL, &TcpGossipApp::ForwardPendingMessages, this);
        }
    }
    
    void ForwardPendingMessages() {
        if (m_pendingMessages.empty()) return;
        
        // Process all pending messages
        for (const auto& msg : m_pendingMessages) {
            if (!m_messageManager.IsForwarded(msg)) {
                ForwardMessage(msg);
                m_messageManager.MarkForwarded(msg);
            }
        }
        
        // Clear pending messages
        m_pendingMessages.clear();
    }
    
    void ForwardMessage(const std::string& msg) {
        // Add newline character to delimit messages
        std::string msgWithDelimiter = msg + "\n";

        // Get active neighbors in priority order
        std::vector<Ipv6Address> priorityNeighbors;
        for (const auto& neighbor : m_neighbors) {
            if (m_connectionPool.IsActive(neighbor)) {
                priorityNeighbors.push_back(neighbor);
            }
        }
        
        // Random shuffle to distribute load
        std::random_shuffle(priorityNeighbors.begin(), priorityNeighbors.end());
        
        // Forward to a subset of neighbors to reduce network load
        // The larger the network, the smaller percentage we forward to
        uint32_t forwardCount = std::max(3u, (uint32_t)(priorityNeighbors.size() * 0.5));
        
        for (uint32_t i = 0; i < std::min(forwardCount, (uint32_t)priorityNeighbors.size()); i++) {
            Ipv6Address neighbor = priorityNeighbors[i];
            Ptr<Socket> socket = m_connectionPool.GetSocket(neighbor);
            
            if (socket) {
                Ptr<Packet> packet = Create<Packet>((uint8_t*)msgWithDelimiter.c_str(), msgWithDelimiter.size());
                int bytes = socket->Send(packet);
                
                if (bytes <= 0) {
                    m_connectionPool.SetSocketActive(neighbor, false);
                }
            }
        }
    }

    void ConnectionSucceeded(Ptr<Socket> socket) {
        Address from;
        socket->GetPeerName(from);
        
        try {
            Inet6SocketAddress inet6Addr = Inet6SocketAddress::ConvertFrom(from);
            Ipv6Address peerAddr = inet6Addr.GetIpv6();
            m_connectionPool.SetSocketActive(peerAddr, true);
        } catch (const std::exception& e) {
            // Address conversion failed, ignore
        }
    }

    void ConnectionFailed(Ptr<Socket> socket) {
        // Let the connection pool handle reconnection
        // on its next management cycle
    }

    void SetSender() { m_isSender = true; }

    uint32_t GetReceivedBlockCount() const {
        return m_messageManager.GetReceivedBlockCount();
    }
    
    uint32_t GetNodeId() const {
        return m_nodeId;
    }
    
    uint32_t GetConnectedNeighborCount() const {
        return m_connectionPool.GetActiveConnectionCount();
    }
};

// Static member initialization
const Time TcpGossipApp::FORWARD_INTERVAL = MilliSeconds(100);
class MinerApp : public Application {
    private:
        EventId m_miningEvent;
        uint32_t m_blockCounter = 0;
        bool m_running = false;
        Ptr<TcpGossipApp> m_gossipApp;
        double m_stopMiningTime = 0.0;
        
        // Global network mining control
        static Time s_lastBlockTime;
        static bool s_blockMinedThisRound;
        static const double GLOBAL_BLOCK_INTERVAL; // 10 seconds
    
    public:
        MinerApp() {}
        
        static uint32_t totalBlocksMined;
        static std::map<uint32_t, uint32_t> perNodeMinedBlocks;
        
        virtual void StartApplication() override {
            m_running = true;
            NS_LOG_INFO("MinerApp started on node " << GetNode()->GetId());
            
            // Stagger mining start times based on node ID
            double startDelay = 1.0 + (GetNode()->GetId() * 0.1) + (((double)rand() / RAND_MAX) * 2.0);
            Simulator::Schedule(Seconds(startDelay), &MinerApp::ScheduleNextMining, this);
        }
        
        virtual void StopApplication() override {
            m_running = false;
            if (m_miningEvent.IsRunning()) {
                Simulator::Cancel(m_miningEvent);
            }
        }
        
        void SetSimulationStopTime(double stopTime) {
            m_stopMiningTime = stopTime - 35.0;  // Stop mining 35s before end to allow propagation
        }
        
        void SetGossipApp(Ptr<TcpGossipApp> app) {
            m_gossipApp = app;
        }
        
        uint32_t GetBlocksMined() const {
            return m_blockCounter;
        }
        
        // Called when a node receives a block from another node
        void OnBlockReceived() {
            // Reset the mining round when a block is received from another node
            s_blockMinedThisRound = true;
            s_lastBlockTime = Simulator::Now();
            
            // Reschedule mining for next round
            if (m_miningEvent.IsRunning()) {
                Simulator::Cancel(m_miningEvent);
            }
            ScheduleNextMining();
        }
        
    private:
        void ScheduleNextMining() {
            if (!m_running) return;
            
            // Calculate time until next global mining round
            Time now = Simulator::Now();
            Time elapsed = now - s_lastBlockTime;
            double timeToNextRound = GLOBAL_BLOCK_INTERVAL - elapsed.GetSeconds();
            
            // If we're close to the next round, schedule for next round
            if (timeToNextRound <= 0) {
                // Reset block mined flag for new round if needed
                if (elapsed.GetSeconds() >= GLOBAL_BLOCK_INTERVAL) {
                    s_blockMinedThisRound = false;
                    s_lastBlockTime = now;
                }
                
                // Add a small random offset to avoid exact synchronization
                double jitter = ((double)rand() / RAND_MAX) * 0.5;
                timeToNextRound = jitter;
            }
            
            // Check if we're still within mining time
            double nextMiningTime = now.GetSeconds() + timeToNextRound;
            if (nextMiningTime < m_stopMiningTime) {
                m_miningEvent = Simulator::Schedule(Seconds(timeToNextRound), &MinerApp::MineBlock, this);
            }
        }
        
        void MineBlock() {
            if (!m_running || Simulator::Now().GetSeconds() >= m_stopMiningTime) {
                return;
            }
            
            // Only attempt to mine if no block has been mined this round
            if (!s_blockMinedThisRound) {
                // Calculate mining probability based on network size
                // Assuming all nodes have equal hash power
                uint32_t __totalNodes = 100; // Adjust based on your network size
                double probability = 1.0 / __totalNodes;
                
                // Try to mine a block
                if (((double)rand() / RAND_MAX) < probability) {
                    // Successfully mined a block
                    m_blockCounter++;
                    totalBlocksMined++;
                    perNodeMinedBlocks[GetNode()->GetId()]++;
                    
                    std::string blockMsg = "Block_" + std::to_string(totalBlocksMined) + "_" + std::to_string(GetNode()->GetId());
                    
                    // Mark that a block has been mined this round
                    s_blockMinedThisRound = true;
                    s_lastBlockTime = Simulator::Now();
                    
                    // Propagate the block to other nodes
                    if (m_gossipApp) {
                        m_gossipApp->SendMessage(blockMsg);
                    }
                    
                    NS_LOG_INFO("Node " << GetNode()->GetId() << " mined block " << totalBlocksMined 
                               << " at time " << Simulator::Now().GetSeconds());
                }
            }
            
            // Schedule next mining attempt
            ScheduleNextMining();
        }
    };
    
    // Initialize static members
    Time MinerApp::s_lastBlockTime = Seconds(0);
    bool MinerApp::s_blockMinedThisRound = false;
    const double MinerApp::GLOBAL_BLOCK_INTERVAL = 10.0; // 10 seconds between blocks
    uint32_t MinerApp::totalBlocksMined = 0;
    std::map<uint32_t, uint32_t> MinerApp::perNodeMinedBlocks;
// Efficient small-world network creation for large networks
void CreateSmallWorldNetwork(std::vector<Ptr<TcpGossipApp>>& gossipApps, 
                            const Ipv6InterfaceContainer& interfaces,
                            uint32_t numNodes, uint32_t numPeers, 
                            double rewireProbability = 0.1) {
    std::cout << "Creating small-world network with " << numNodes << " nodes, " 
              << numPeers << " peers per node, and rewire probability " 
              << rewireProbability << std::endl;
    
    // Create neighborhood data structures efficiently
    std::vector<std::unordered_set<uint32_t>> neighbors(numNodes);
    
    // Step 1: Create a regular ring lattice
    for (uint32_t i = 0; i < numNodes; i++) {
        for (uint32_t j = 1; j <= numPeers / 2; j++) {
            // Connect to j nodes clockwise
            uint32_t clockwise = (i + j) % numNodes;
            // Connect to j nodes counter-clockwise
            uint32_t counterClockwise = (i - j + numNodes) % numNodes;
            
            neighbors[i].insert(clockwise);
            neighbors[i].insert(counterClockwise);
        }
    }
    
    // Step 2: Rewire some connections with probability p
    for (uint32_t i = 0; i < numNodes; i++) {
        std::vector<uint32_t> toRewire;
        
        // Identify connections to potentially rewire
        for (uint32_t j = 1; j <= numPeers / 2; j++) {
            uint32_t clockwise = (i + j) % numNodes;
            
            // With probability p, mark this connection for rewiring
            if ((double)rand() / RAND_MAX < rewireProbability) {
                toRewire.push_back(clockwise);
            }
        }
        
        // Perform rewiring for this node
        for (uint32_t target : toRewire) {
            // Remove the original connection
            neighbors[i].erase(target);
            
            // Find a new random node that's not already connected
            uint32_t attempts = 0;
            uint32_t randomNode;
            bool found = false;
            
            while (attempts < 10 && !found) { // Limit attempts to avoid infinite loops
                randomNode = rand() % numNodes;
                
                if (randomNode != i && neighbors[i].find(randomNode) == neighbors[i].end()) {
                    found = true;
                }
                attempts++;
            }
            
            if (found) {
                neighbors[i].insert(randomNode);
            } else {
                // If we couldn't find a suitable new neighbor, keep the original
                neighbors[i].insert(target);
            }
        }
    }
    
    // Now convert the neighbor sets to actual network connections
    for (uint32_t i = 0; i < numNodes; i++) {
        for (uint32_t neighbor : neighbors[i]) {
            gossipApps[i]->AddNeighbor(interfaces.GetAddress(neighbor, 1));
        }
    }
    
    std::cout << "Small-world network topology created successfully" << std::endl;
}

// Add this function before your main() function
void PrintSimulationProgress(double interval) {
    std::cout << "Simulation time passed: " << Simulator::Now().GetSeconds() << " secs" << std::endl;
    
    // Schedule the next progress update
    Simulator::Schedule(Seconds(interval), &PrintSimulationProgress, interval);
}


// Add this class before the main() function
class NetworkMonitor {
    private:
        std::vector<Ptr<TcpGossipApp>>& m_gossipApps;
        std::vector<Ptr<MinerApp>>& m_minerApps;
        uint32_t m_numNodes;
        EventId m_reportEvent;
        double m_reportInterval;
        
        // Store historical data for reporting
        struct ReportData {
            double timestamp;
            uint32_t totalBlocksMined;
            uint32_t totalBlocksReceived;
            double avgBlocksPropagated;
            uint32_t minBlocksReceived;
            uint32_t maxBlocksReceived;
        };
        
        std::vector<ReportData> m_reportHistory;
        
    public:
        NetworkMonitor(std::vector<Ptr<TcpGossipApp>>& gossipApps,
                       std::vector<Ptr<MinerApp>>& minerApps,
                       double reportInterval)
            : m_gossipApps(gossipApps), 
              m_minerApps(minerApps),
              m_reportInterval(reportInterval) 
        {
            m_numNodes = gossipApps.size();
        }
        
        void Start() {
            // Schedule the first report
            m_reportEvent = Simulator::Schedule(Seconds(m_reportInterval), 
                                               &NetworkMonitor::GenerateReport, 
                                               this);
        }
        
        void Stop() {
            if (m_reportEvent.IsRunning()) {
                Simulator::Cancel(m_reportEvent);
            }
            
            // Generate final summary
            PrintSummary();
        }
        
    private:
        void GenerateReport() {
            double currentTime = Simulator::Now().GetSeconds();
            uint32_t totalBlocksMined = MinerApp::totalBlocksMined;
            
            // Calculate block propagation statistics
            uint32_t totalReceivedBlocks = 0;
            uint32_t minReceivedBlocks = UINT32_MAX;
            uint32_t maxReceivedBlocks = 0;
            
            for (uint32_t i = 0; i < m_numNodes; i++) {
                uint32_t receivedBlocks = m_gossipApps[i]->GetReceivedBlockCount();
                totalReceivedBlocks += receivedBlocks;
                minReceivedBlocks = std::min(minReceivedBlocks, receivedBlocks);
                maxReceivedBlocks = std::max(maxReceivedBlocks, receivedBlocks);
            }
            
            double avgReceivedBlocks = static_cast<double>(totalReceivedBlocks) / m_numNodes;
            double propagationRatio = (totalBlocksMined > 0) ? 
                                     (avgReceivedBlocks / totalBlocksMined) * 100.0 : 0.0;
            
            // Store the report data
            ReportData report;
            report.timestamp = currentTime;
            report.totalBlocksMined = totalBlocksMined;
            report.totalBlocksReceived = totalReceivedBlocks;
            report.avgBlocksPropagated = avgReceivedBlocks;
            report.minBlocksReceived = minReceivedBlocks;
            report.maxBlocksReceived = maxReceivedBlocks;
            
            m_reportHistory.push_back(report);
            
            // Print the current report
            std::cout << "\n=== NETWORK REPORT AT " << std::setprecision(1) << currentTime << " SECONDS ===" << std::endl;
            std::cout << "Total blocks mined: " << totalBlocksMined << std::endl;
            std::cout << "Block propagation:" << std::endl;
            std::cout << "  Average blocks received per node: " << std::setprecision(2) << avgReceivedBlocks << std::endl;
            std::cout << "  Propagation efficiency: " << std::setprecision(2) << propagationRatio << "%" << std::endl;
            std::cout << "  Min blocks received: " << minReceivedBlocks << std::endl;
            std::cout << "  Max blocks received: " << maxReceivedBlocks << std::endl;
            std::cout << "=================================================" << std::endl;
            
            // Schedule the next report
            m_reportEvent = Simulator::Schedule(Seconds(m_reportInterval), 
                                               &NetworkMonitor::GenerateReport, 
                                               this);
        }
        
        void PrintSummary() {
            if (m_reportHistory.empty()) return;
            
            std::cout << "\n\n========== SUMMARY OF NETWORK REPORTS ==========" << std::endl;
            std::cout << "Time(s)\tBlocks Mined\tAvg Blocks Received\tPropagation %" << std::endl;
            
            for (const auto& report : m_reportHistory) {
                double propagationRatio = (report.totalBlocksMined > 0) ? 
                                         (report.avgBlocksPropagated / report.totalBlocksMined) * 100.0 : 0.0;
                                         
                std::cout << std::setprecision(1) << report.timestamp << "\t"
                          << report.totalBlocksMined << "\t\t"
                          << std::setprecision(2) << report.avgBlocksPropagated << "\t\t\t"
                          << std::setprecision(2) << propagationRatio << "%" << std::endl;
            }
            
            std::cout << "=================================================" << std::endl;
        }
    };


int main(int argc, char* argv[]) {
    // Seed the random number generator with current time
    srand(time(nullptr));
    CommandLine cmd;
    uint32_t numNodes = 1000;
    uint32_t numPeers = 5;
    double rewireProbability = 0.1;
    double simulationTime = 500.0;

    cmd.AddValue("numNodes", "Number of nodes in the network", numNodes);
    cmd.AddValue("numPeers", "Number of peers per node", numPeers);
    cmd.AddValue("rewireProbability", "Probability to rewire an edge in small-world model", rewireProbability);
    cmd.AddValue("simTime", "Simulation time in seconds", simulationTime);
    cmd.Parse(argc, argv);

    // Force decimal point display to avoid locale issues
    std::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);

    // Print simulation parameters
    std::cout << "Starting TCP Gossip simulation with:" << std::endl;
    std::cout << "  Number of nodes: " << numNodes << std::endl;
    std::cout << "  Peers per node: " << numPeers << std::endl;
    std::cout << "  Rewire probability: " << rewireProbability << std::endl;
    std::cout << "  Simulation time: " << simulationTime << " seconds" << std::endl;

    // Set up simulation environment
    Time::SetResolution(Time::NS);
    LogComponentEnable("TcpGossip", LOG_LEVEL_INFO);

    // Create nodes
    NodeContainer nodes;
    nodes.Create(numNodes);

    // Set up internet stack
    InternetStackHelper internet;
    internet.Install(nodes);

    // Create CSMA links between nodes
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(10)));
    
    NetDeviceContainer devices = csma.Install(nodes);

    // Assign IPv6 addresses
    Ipv6AddressHelper ipv6;
    ipv6.SetBase(Ipv6Address("2001:db8::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer interfaces = ipv6.Assign(devices);
    
    // Collect interfaces for each node
    std::vector<Ptr<TcpGossipApp>> gossipApps;
    std::vector<Ptr<MinerApp>> minerApps;
    
    // Create gossip applications on each node
    for (uint32_t i = 0; i < numNodes; i++) {
        // Create and install the gossip app
        Ptr<TcpGossipApp> app = CreateObject<TcpGossipApp>(interfaces.GetAddress(i, 1));
        nodes.Get(i)->AddApplication(app);
        app->SetStartTime(Seconds(1.0));
        app->SetStopTime(Seconds(simulationTime));
        gossipApps.push_back(app);
        
        // Create and install the miner app with a delay
        Ptr<MinerApp> minerApp = CreateObject<MinerApp>();
        nodes.Get(i)->AddApplication(minerApp);
        minerApp->SetStartTime(Seconds(10.0));
        minerApp->SetStopTime(Seconds(simulationTime));
        minerApp->SetGossipApp(app);
        minerApp->SetSimulationStopTime(simulationTime);
        minerApps.push_back(minerApp);
    }
    
    // Create the small-world network topology
    CreateSmallWorldNetwork(gossipApps, interfaces, numNodes, numPeers, rewireProbability);
    
    // // Pick a subset of nodes to act as message senders
    // uint32_t numSenders = std::max(5u, numNodes / 100);
    // std::cout << "Selecting " << numSenders << " nodes as initial block producers" << std::endl;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t senderIndex = rand() % numNodes;
        gossipApps[senderIndex]->SetSender();
    }
    

   // Create the network monitor
    NetworkMonitor monitor(gossipApps, minerApps, 30.0);

    // Schedule when to start and stop monitoring
    Simulator::Schedule(Seconds(5.0), &NetworkMonitor::Start, &monitor);
    Simulator::Schedule(Seconds(simulationTime - 1.0), &NetworkMonitor::Stop, &monitor);

    // Schedule progress reporting
    Simulator::Schedule(Seconds(1.0), &PrintSimulationProgress, 1.0);

    // Configure when to stop the simulation
    Simulator::Stop(Seconds(simulationTime));

    // Run the simulation
    std::cout << "Running simulation for " << simulationTime << " seconds..." << std::endl;
    Simulator::Run(); 
    // Collect results
    std::cout << "\nSimulation completed. Results:" << std::endl;
    std::cout << "Total blocks mined: " << MinerApp::totalBlocksMined << std::endl;
    
    // Calculate block propagation statistics
    uint32_t totalReceivedBlocks = 0;
    uint32_t minReceivedBlocks = UINT32_MAX;
    uint32_t maxReceivedBlocks = 0;
    
    std::map<uint32_t, uint32_t> blockReceiptDistribution;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t receivedBlocks = gossipApps[i]->GetReceivedBlockCount();
        totalReceivedBlocks += receivedBlocks;
        minReceivedBlocks = std::min(minReceivedBlocks, receivedBlocks);
        maxReceivedBlocks = std::max(maxReceivedBlocks, receivedBlocks);
        
        blockReceiptDistribution[receivedBlocks]++;
    }
    
    double avgReceivedBlocks = static_cast<double>(totalReceivedBlocks) / numNodes;
    double propagationRatio = (avgReceivedBlocks / MinerApp::totalBlocksMined) * 100.0;
    
    std::cout << "Block propagation statistics:" << std::endl;
    std::cout << "  Average blocks received per node: " << std::setprecision(2) << avgReceivedBlocks 
              << " (" << std::setprecision(2) << propagationRatio << "% of total blocks)" << std::endl;
    std::cout << "  Min blocks received: " << minReceivedBlocks << std::endl;
    std::cout << "  Max blocks received: " << maxReceivedBlocks << std::endl;
    
    // Print distribution of block receipt
    std::cout << "\nBlock receipt distribution:" << std::endl;
    for (auto& pair : blockReceiptDistribution) {
        double percentage = static_cast<double>(pair.second) / numNodes * 100.0;
        std::cout << "  " << pair.first << " blocks: " << pair.second << " nodes (" 
                  << std::setprecision(2) << percentage << "%)" << std::endl;
    }
    
        
    // Print blocks mined by each node in ascending order by node ID
    std::cout << "\nBlocks mined by each node:" << std::endl;
    std::vector<std::pair<uint32_t, uint32_t>> minerStats;
    for (auto& pair : MinerApp::perNodeMinedBlocks) {
        minerStats.push_back(pair);
    }

    // Sort by node ID in ascending order
    std::sort(minerStats.begin(), minerStats.end(), 
            [](const auto& a, const auto& b) { return a.first < b.first; });

    // Print all nodes
    for (const auto& pair : minerStats) {
        std::cout << "  Node " << pair.first << ": " << pair.second << " blocks" << std::endl;
    }
    
    // Calculate network connectivity statistics
    uint32_t totalConnections = 0;
    uint32_t minConnections = UINT32_MAX;
    uint32_t maxConnections = 0;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t connectionCount = gossipApps[i]->GetConnectedNeighborCount();
        totalConnections += connectionCount;
        minConnections = std::min(minConnections, connectionCount);
        maxConnections = std::max(maxConnections, connectionCount);
    }
    
    double avgConnections = static_cast<double>(totalConnections) / numNodes;
    
    std::cout << "\nNetwork connectivity statistics:" << std::endl;
    std::cout << "  Average active connections per node: " << std::setprecision(2) << avgConnections << std::endl;
    std::cout << "  Min active connections: " << minConnections << std::endl;
    std::cout << "  Max active connections: " << maxConnections << std::endl;
    
    Simulator::Destroy();
    
    return 0;
}
