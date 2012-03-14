#include "precomp.h"

#ifdef _WIN32
#include <ws2ipdef.h>
#endif

#ifdef _WIN32
DWORD get_interfaces_windows_mib(Remote *remote, Packet *response)
{
	DWORD result = ERROR_SUCCESS;
	DWORD tlv_cnt;

	Tlv entries[6];
	PMIB_IPADDRTABLE table = NULL;
	DWORD tableSize = sizeof(MIB_IPADDRROW) * 33;
	DWORD index;
	DWORD mtu_bigendian;
	DWORD interface_index_bigendian;
	MIB_IFROW iface;

	do
	{
		// Allocate memory for reading addresses into
		if (!(table = (PMIB_IPADDRTABLE)malloc(tableSize)))
		{
			result = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}

		// Get the IP address table
		if (GetIpAddrTable(table, &tableSize, TRUE) != NO_ERROR)
		{
			result = GetLastError();
			break;
		}

		// Enumerate the entries
		for (index = 0; index < table->dwNumEntries; index++)
		{
			tlv_cnt = 0;

			interface_index_bigendian = htonl(table->table[index].dwIndex);
			entries[tlv_cnt].header.length = sizeof(DWORD);
			entries[tlv_cnt].header.type = TLV_TYPE_INTERFACE_INDEX;
			entries[tlv_cnt].buffer = (PUCHAR)&interface_index_bigendian;
			tlv_cnt++;

			entries[tlv_cnt].header.length = sizeof(DWORD);
			entries[tlv_cnt].header.type = TLV_TYPE_IP;
			entries[tlv_cnt].buffer = (PUCHAR)&table->table[index].dwAddr;
			tlv_cnt++;

			entries[tlv_cnt].header.length = sizeof(DWORD);
			entries[tlv_cnt].header.type = TLV_TYPE_NETMASK;
			entries[tlv_cnt].buffer = (PUCHAR)&table->table[index].dwMask;
			tlv_cnt++;

			iface.dwIndex = table->table[index].dwIndex;

			// If interface information can get gotten, use it.
			if (GetIfEntry(&iface) == NO_ERROR)
			{
				entries[tlv_cnt].header.length = iface.dwPhysAddrLen;
				entries[tlv_cnt].header.type = TLV_TYPE_MAC_ADDR;
				entries[tlv_cnt].buffer = (PUCHAR)iface.bPhysAddr;
				tlv_cnt++;

				mtu_bigendian = htonl(iface.dwMtu);
				entries[tlv_cnt].header.length = sizeof(DWORD);
				entries[tlv_cnt].header.type = TLV_TYPE_INTERFACE_MTU;
				entries[tlv_cnt].buffer = (PUCHAR)&mtu_bigendian;
				tlv_cnt++;

				if (iface.bDescr)
				{
					entries[tlv_cnt].header.length = iface.dwDescrLen + 1;
					entries[tlv_cnt].header.type = TLV_TYPE_MAC_NAME;
					entries[tlv_cnt].buffer = (PUCHAR)iface.bDescr;
					tlv_cnt++;
				}
			}

			// Add the interface group
			packet_add_tlv_group(response, TLV_TYPE_NETWORK_INTERFACE,
			entries, tlv_cnt);
		}

	} while (0);

	if (table)
		free(table);

	return result;
}


DWORD get_interfaces_windows(Remote *remote, Packet *response) {
	DWORD result = ERROR_SUCCESS;
	DWORD tlv_cnt;
	// Most of the time we'll need:
	//   index, name (description), MAC addr, mtu, flags, IP addr, netmask, maybe scope id
	// In some cases, the interface will have multiple addresses, so we'll realloc
	// this when necessary, but this will cover the common case.
	DWORD allocd_entries = 10;
	Tlv *entries = (Tlv *)malloc(sizeof(Tlv) * 10);

	DWORD mtu_bigendian;
	DWORD interface_index_bigendian;

	ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_DNS_SERVER;

	LPSOCKADDR sockaddr;

	ULONG family = AF_UNSPEC;
	IP_ADAPTER_ADDRESSES *pAdapters = NULL;
	IP_ADAPTER_ADDRESSES *pCurr = NULL;
	ULONG outBufLen = 0;
	DWORD (WINAPI *gaa)(DWORD, DWORD, void *, void *, void *);

	// Use the larger version so we're guaranteed to have a large enough struct
	IP_ADAPTER_UNICAST_ADDRESS_LH *pAddr;


	do
	{
		gaa = (DWORD (WINAPI *)(DWORD,DWORD,void*,void*,void*))GetProcAddress(
				GetModuleHandle("iphlpapi"), "GetAdaptersAddresses"
			);
		if (!gaa) {
			result = get_interfaces_windows_mib(remote, response);
			break;
		}

		gaa(family, flags, NULL, pAdapters, &outBufLen);
		if (!(pAdapters = (IP_ADAPTER_ADDRESSES *)malloc(outBufLen)))
		{
			result = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}
		if (gaa(family, flags, NULL, pAdapters, &outBufLen))
		{
			result = GetLastError();
			break;
		}

		// Enumerate the entries
		for (pCurr = pAdapters; pCurr; pCurr = pCurr->Next)
		{
			tlv_cnt = 0;

			interface_index_bigendian = htonl(pCurr->IfIndex);
			entries[tlv_cnt].header.length = sizeof(DWORD);
			entries[tlv_cnt].header.type   = TLV_TYPE_INTERFACE_INDEX;
			entries[tlv_cnt].buffer        = (PUCHAR)&interface_index_bigendian;
			tlv_cnt++;

			entries[tlv_cnt].header.length = pCurr->PhysicalAddressLength;
			entries[tlv_cnt].header.type   = TLV_TYPE_MAC_ADDR;
			entries[tlv_cnt].buffer        = (PUCHAR)pCurr->PhysicalAddress;
			tlv_cnt++;

			entries[tlv_cnt].header.length = wcslen(pCurr->Description)*2 + 1;
			entries[tlv_cnt].header.type   = TLV_TYPE_MAC_NAME;
			entries[tlv_cnt].buffer        = (PUCHAR)pCurr->Description;
			tlv_cnt++;

			mtu_bigendian            = htonl(pCurr->Mtu);
			entries[tlv_cnt].header.length = sizeof(DWORD);
			entries[tlv_cnt].header.type   = TLV_TYPE_INTERFACE_MTU;
			entries[tlv_cnt].buffer        = (PUCHAR)&mtu_bigendian;
			tlv_cnt++;


			for (pAddr = (void*)pCurr->FirstUnicastAddress; pAddr; pAddr = (void*)pAddr->Next)
			{
				// This loop can add up to three Tlv's - one for address, one for scope_id, one for netmask.
				// Go ahead and allocate enough room for all of them.
				if (allocd_entries < tlv_cnt+3) {
					entries = realloc(entries, sizeof(Tlv) * (tlv_cnt+3));
					allocd_entries += 3;
				}

				sockaddr = pAddr->Address.lpSockaddr;
				if (sockaddr->sa_family == AF_INET) {
					entries[tlv_cnt].header.length = 4;
					entries[tlv_cnt].header.type   = TLV_TYPE_IP;
					entries[tlv_cnt].buffer        = (PUCHAR)&(((struct sockaddr_in *)sockaddr)->sin_addr);
					if (pCurr->Length > 68) {
						tlv_cnt++;
						entries[tlv_cnt].header.length = 4;
						entries[tlv_cnt].header.type   = TLV_TYPE_NETMASK;
						entries[tlv_cnt].buffer        = (PUCHAR)&(((struct sockaddr_in *)sockaddr)->sin_addr);
					}
				} else {
					entries[tlv_cnt].header.length = 16;
					entries[tlv_cnt].header.type   = TLV_TYPE_IP;
					entries[tlv_cnt].buffer        = (PUCHAR)&(((struct sockaddr_in6 *)sockaddr)->sin6_addr);

					tlv_cnt++;
					entries[tlv_cnt].header.length = sizeof(DWORD);
					entries[tlv_cnt].header.type   = TLV_TYPE_IP6_SCOPE;
					entries[tlv_cnt].buffer        = (PUCHAR)&(((struct sockaddr_in6 *)sockaddr)->sin6_scope_id);

					if (pCurr->Length > 68) {
						tlv_cnt++;
						entries[tlv_cnt].header.length = 16;
						entries[tlv_cnt].header.type   = TLV_TYPE_NETMASK;
						entries[tlv_cnt].buffer        = (PUCHAR)&(((struct sockaddr_in6 *)sockaddr)->sin6_addr);
					}

				}
				tlv_cnt++;
			}
			// Add the interface group
			packet_add_tlv_group(response, TLV_TYPE_NETWORK_INTERFACE,
					entries, tlv_cnt);
		}
	} while (0);

	if (entries)
		free(entries);
	if (pAdapters)
		free(pAdapters);

	return result;
}

#else
int get_interfaces_linux(Remote *remote, Packet *response) {
	struct ifaces_list *ifaces = NULL;
	int i;
	int result;
	uint32_t interface_index_bigendian, mtu_bigendian;
	DWORD allocd_entries = 10;
	Tlv *entries = (Tlv *)malloc(sizeof(Tlv) * 10);

	dprintf("Grabbing interfaces");
	result = netlink_get_interfaces(&ifaces);
	dprintf("Got 'em");

	if (!result) {
		for (i = 0; i < ifaces->entries; i++) {
			int tlv_cnt = 0;
			int j = 0;
			dprintf("Building TLV for iface %d", i);

			entries[tlv_cnt].header.length = strlen(ifaces->ifaces[i].name)+1;
			entries[tlv_cnt].header.type   = TLV_TYPE_MAC_NAME;
			entries[tlv_cnt].buffer        = (PUCHAR)ifaces->ifaces[i].name;
			tlv_cnt++;

			entries[tlv_cnt].header.length = 6;
			entries[tlv_cnt].header.type   = TLV_TYPE_MAC_ADDR;
			entries[tlv_cnt].buffer        = (PUCHAR)ifaces->ifaces[i].hwaddr;
			tlv_cnt++;

			mtu_bigendian            = htonl(ifaces->ifaces[i].mtu);
			entries[tlv_cnt].header.length = sizeof(uint32_t);
			entries[tlv_cnt].header.type   = TLV_TYPE_INTERFACE_MTU;
			entries[tlv_cnt].buffer        = (PUCHAR)&mtu_bigendian;
			tlv_cnt++;

			entries[tlv_cnt].header.length = strlen(ifaces->ifaces[i].flags)+1;
			entries[tlv_cnt].header.type   = TLV_TYPE_INTERFACE_FLAGS;
			entries[tlv_cnt].buffer        = (PUCHAR)ifaces->ifaces[i].flags;
			tlv_cnt++;

			interface_index_bigendian = htonl(ifaces->ifaces[i].index);
			entries[tlv_cnt].header.length = sizeof(uint32_t);
			entries[tlv_cnt].header.type   = TLV_TYPE_INTERFACE_INDEX;
			entries[tlv_cnt].buffer        = (PUCHAR)&interface_index_bigendian;
			tlv_cnt++;

			for (j = 0; j < ifaces->ifaces[i].addr_count; j++) {
				if (allocd_entries < tlv_cnt+3) {
					entries = realloc(entries, sizeof(Tlv) * (tlv_cnt+3));
					allocd_entries += 3;
				}
				if (ifaces->ifaces[i].addr_list[j].family == AF_INET) {
					dprintf("ip addr for %s", ifaces->ifaces[i].name);
					entries[tlv_cnt].header.length = sizeof(__u32);
					entries[tlv_cnt].header.type   = TLV_TYPE_IP;
					entries[tlv_cnt].buffer        = (PUCHAR)&ifaces->ifaces[i].addr_list[j].ip.addr;
					tlv_cnt++;

					//dprintf("netmask for %s", ifaces->ifaces[i].name);
					entries[tlv_cnt].header.length = sizeof(__u32);
					entries[tlv_cnt].header.type   = TLV_TYPE_NETMASK;
					entries[tlv_cnt].buffer        = (PUCHAR)&ifaces->ifaces[i].addr_list[j].nm.netmask;
					tlv_cnt++;
				} else {
					dprintf("-- ip six addr for %s", ifaces->ifaces[i].name);
					entries[tlv_cnt].header.length = sizeof(__u128);
					entries[tlv_cnt].header.type   = TLV_TYPE_IP;
					entries[tlv_cnt].buffer        = (PUCHAR)&ifaces->ifaces[i].addr_list[j].ip.addr6;
					tlv_cnt++;

					//dprintf("netmask6 for %s", ifaces->ifaces[i].name);
					entries[tlv_cnt].header.length = sizeof(__u128);
					entries[tlv_cnt].header.type   = TLV_TYPE_NETMASK;
					entries[tlv_cnt].buffer        = (PUCHAR)&ifaces->ifaces[i].addr_list[j].nm.netmask6;
					tlv_cnt++;
				}
			}

			dprintf("Adding TLV to group");
			packet_add_tlv_group(response, TLV_TYPE_NETWORK_INTERFACE, entries, tlv_cnt);
			dprintf("done Adding TLV to group");
		}
	}

	if (ifaces)
		free(ifaces);
	if (entries)
		free(entries);


	return result;
}
#endif


/*
 * Returns zero or more local interfaces to the requestor
 */
DWORD request_net_config_get_interfaces(Remote *remote, Packet *packet)
{
	Packet *response = packet_create_response(packet);
	DWORD result = ERROR_SUCCESS;

#ifdef _WIN32
	result = get_interfaces_windows(remote, response);
#else
	result = get_interfaces_linux(remote, response);
#endif

	// Transmit the response if valid
	packet_transmit_response(result, remote, response);

	return result;
}


