/*
 * Data and acknowledgment packet pools
 */
#include "datapacket.h"

PacketDB<DataPacket> DataPacket::_packetdb;
PacketDB<DataAck> DataAck::_packetdb;
