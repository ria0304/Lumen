#ifndef NIC_H
#define NIC_H
#include <stdint.h>
int nic_init(void);
int nic_present(void);
const uint8_t *nic_mac(void);
uint16_t nic_iobase(void);
int nic_send(const void *frame, uint32_t len);
int nic_recv(void *out, uint32_t max, uint32_t timeout_ticks);
int nic_poll_rx(void);
uint32_t nic_tx_count(void);
uint32_t nic_rx_count(void);
void nic_dump(void);
/* L3 helpers */
uint16_t ip_checksum(const void *data, uint32_t len);
int arp_lookup(uint32_t ip, uint8_t *mac_out);
void arp_add(uint32_t ip, const uint8_t *mac);
int nic_ping_ip(uint32_t ip);
int nic_run_self_test(void);
#endif
