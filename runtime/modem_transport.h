/* Platform transport only: phonebook entries and telnet live in modem.c. */
#ifndef VC_MODEM_TRANSPORT_H
#define VC_MODEM_TRANSPORT_H

#include "modem.h"

const ModemTransport *modem_transport_ops(void);

#endif
