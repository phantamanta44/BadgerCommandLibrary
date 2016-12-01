#include "ServiceMaster.hpp"
#include "Service.hpp"
#include "ControlServicePackets.h"
#include <cstdint>
#include <thread>

using namespace BCL;

constexpr int READ_BUFFER_SIZE = 256;

ServiceMaster::ServiceMaster(int robot_id, UdpSocket *udpSocket, SerialPort *serialPort)
{
    this->robotID = static_cast<uint8_t>(robot_id);
    this->isRunning = false;
    this->socket = udpSocket;
    this->serialPort = serialPort;
}

ServiceMaster::~ServiceMaster()
{
}

uint8_t ServiceMaster::GetRobotID() const
{
    return this->robotID;
}

void ServiceMaster::AddService(Service *s)
{
    this->services.push_back(s);
    s->SetServiceMaster(this);
    if (this->isRunning && s->IsActive())
        s->ExecuteOnTime();
}

void ServiceMaster::AddEndpoint(int robot_id, struct sockaddr_in addr)
{
    this->robotEndpointMap[static_cast<uint8_t>(robot_id)] = addr;
}

BCL_STATUS ServiceMaster::SendPacketSerial(BclPacket *pkt)
{
    uint8_t buffer[255];
    uint8_t bytes_written;
    BCL_STATUS status;

    if (pkt == nullptr)
        return BCL_INVALID_PARAMETER;

    if (this->serialPort == nullptr)
        return BCL_SERIAL_ERROR;

    status = SerializeBclPacket(pkt, buffer, 255, &bytes_written);
    if (status != BCL_OK)
        return status;

    this->serialPort->Write(buffer, bytes_written);
    return BCL_OK;
}

BCL_STATUS ServiceMaster::SendPacketUdp(BclPacket *pkt)
{
    uint8_t buffer[255];
    uint8_t bytes_written;
    BCL_STATUS status;

    if (pkt == nullptr)
        return BCL_INVALID_PARAMETER;

    if (this->socket == nullptr)
        return BCL_SOCKET_ERROR;

    status = SerializeBclPacket(pkt, buffer, 255, &bytes_written);
    if (status != BCL_OK)
        return status;

    // TODO: handle broadcast UDP address

    int robot_id = pkt->Header.Destination.RobotID;
    if (this->robotEndpointMap.find(robot_id) == this->robotEndpointMap.end())
        return BCL_NOT_FOUND;

    struct sockaddr_in dest_addr = this->robotEndpointMap[robot_id];

    if (this->socket->Write(buffer, bytes_written, (struct sockaddr *) &dest_addr) != bytes_written)
        return BCL_SOCKET_ERROR;

    return BCL_OK;
}

void ServiceMaster::SerialReader()
{
    uint8_t read_buffer[READ_BUFFER_SIZE];
    memset(read_buffer, 0, READ_BUFFER_SIZE);

    while (true)
    {
        int bytes_read = this->serialPort->Read(read_buffer, READ_BUFFER_SIZE);
        if (bytes_read < 0)
            continue;

        this->PacketHandler(read_buffer, static_cast<uint8_t>(bytes_read));
        memset(read_buffer, 0, READ_BUFFER_SIZE);
    }
}

void ServiceMaster::UdpSocketReader()
{
    uint8_t read_buffer[READ_BUFFER_SIZE];
    memset(read_buffer, 0, READ_BUFFER_SIZE);

    while (true)
    {
        struct sockaddr_in src_addr;
        BclPacketHeader pkt_hdr;

        int bytes_read = socket->Read(read_buffer, READ_BUFFER_SIZE, (struct sockaddr *) &src_addr);
        if (bytes_read < 0)
            continue;

        memset(&pkt_hdr, 0, sizeof(BclPacketHeader));

        // if a valid header is received from an unknown address
        // add to robotEndpointMap
        if (ParseBclHeader(&pkt_hdr, read_buffer, static_cast<uint8_t>(bytes_read)) == BCL_OK)
        {
            uint8_t incoming_robot_id = pkt_hdr.Source.RobotID;
            if (this->robotEndpointMap.find(incoming_robot_id) == this->robotEndpointMap.end())
                this->robotEndpointMap[incoming_robot_id] = src_addr;
        }

        this->PacketHandler(read_buffer, static_cast<uint8_t>(bytes_read));
        memset(read_buffer, 0, READ_BUFFER_SIZE);
    }
}

void ServiceMaster::PacketHandler(const uint8_t *buffer, uint8_t length)
{
    // TODO: handle access control packets here
    BclPacketHeader hdr;
    memset(&hdr, 0, sizeof(BclPacketHeader));
    if (ParseBclHeader(&hdr, buffer, length) != BCL_OK)
        return;

    // reject packet if ID doesn't match and is not broadcast
    if (hdr.Destination.RobotID != this->robotID && this->robotID != BCL_BROADCAST_ROBOT_ID)
        return;

    BclPacket pkt;
    switch (hdr.Opcode)
    {
        case ACTIVATE_SERVICE_OPCODE:
        case DEACTIVATE_SERVICE_OPCODE:
            for (size_t i = 0; i < this->services.size(); i++)
            {
                if (this->services[i]->GetID() == hdr.Destination.ServiceID)
                {
                    this->services[i]->SetActive(hdr.Opcode == ACTIVATE_SERVICE_OPCODE);
                    return;
                }
            }
                    
            return;

        case QUERY_HEARTBEAT_OPCODE:
            InitializeReportHeartbeatPacket(&pkt);
            pkt.Header.Destination = hdr.Source;
            pkt.Header.Source.RobotID = static_cast<uint8_t>(this->robotID);

            // HACK
            // TODO: Don't just blindly send across both interfaces
            this->SendPacketSerial(&pkt);
            this->SendPacketUdp(&pkt);
            return;

        default:
            break;
    }

    for (auto& s : this->services)
    {
        if (!s->IsActive())
            continue;
        
        if (hdr.Destination.ServiceID != s->GetID())
            continue;

        if (!s->HandlePacket(buffer, length))
            continue;

        if (s->GetSleepInterval() == Service::RUN_ON_PACKET_RECEIVE)
            s->ExecuteOnTime();

        // packet issued to service - done!
        return;
    }
}

void ServiceMaster::Run()
{
    std::thread serialReadThread;
    std::thread udpReadThread;

    if (!socket || !socket->Open())
        return;

    // run the services
    for (auto& s : this->services)
        if (s->GetSleepInterval() != Service::RUN_ON_PACKET_RECEIVE && s->IsActive())
            s->ExecuteOnTime();

    if (this->serialPort)
        serialReadThread = std::thread(&ServiceMaster::SerialReader, this);

    if (this->socket)
        udpReadThread = std::thread(&ServiceMaster::UdpSocketReader, this);

    while (true);
}
