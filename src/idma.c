/* src/idma.c - low level rx and tx management
 *
 * --------------------------------------------------------------------
 *
 * Copyright (C) 2003  ACX100 Open Source Project
 *
 *   The contents of this file are subject to the Mozilla Public
 *   License Version 1.1 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.mozilla.org/MPL/
 *
 *   Software distributed under the License is distributed on an "AS
 *   IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 *   implied. See the License for the specific language governing
 *   rights and limitations under the License.
 *
 *   Alternatively, the contents of this file may be used under the
 *   terms of the GNU Public License version 2 (the "GPL"), in which
 *   case the provisions of the GPL are applicable instead of the
 *   above.  If you wish to allow the use of your version of this file
 *   only under the terms of the GPL and not to allow others to use
 *   your version of this file under the MPL, indicate your decision
 *   by deleting the provisions above and replace them with the notice
 *   and other provisions required by the GPL.  If you do not delete
 *   the provisions above, a recipient may use your version of this
 *   file under either the MPL or the GPL.
 *
 * --------------------------------------------------------------------
 *
 * Inquiries regarding the ACX100 Open Source Project can be
 * made directly to:
 *
 * acx100-users@lists.sf.net
 * http://acx100.sf.net
 *
 * --------------------------------------------------------------------
 */

#include <linux/config.h>
#include <linux/version.h>

#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>

#include <wlan_compat.h>

#include <linux/pci.h>



/*================================================================*/
/* Project Includes */

#include <p80211hdr.h>
#include <p80211mgmt.h>
#include <acx100_conv.h>
#include <acx100.h>
#include <p80211types.h>
#include <acx100_helper.h>
#include <acx100_helper2.h>
#include <idma.h>
#include <ihw.h>
#include <monitor.h>

/* these used to be increased to 32 each,
 * but several people had memory allocation issues,
 * so back to 16 again... */
#if (WLAN_HOSTIF==WLAN_USB)
#define TXBUFFERNO 10
#define RXBUFFERNO 10
#else
#define RXBUFFERNO 16
#define TXBUFFERNO 16
#endif

#define MINFREE_TX 3
spinlock_t tx_lock;
spinlock_t rx_lock;

#ifdef ACX_DEBUG
#if (WLAN_HOSTIF==WLAN_USB)
extern void acx100usb_dump_bytes(void *,int);
#endif
#endif
#if (WLAN_HOSTIF==WLAN_USB)
extern void acx100usb_tx_data(wlandevice_t *,void *);
#endif


/*----------------------------------------------------------------
* acx100_create_dma_regions
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_create_dma_regions(wlandevice_t *priv)
{
	QueueConfig_t qcfg;
	
	acx100_memmap_t MemMap;
	struct TIWLAN_DC *pDc;

	FN_ENTER;

	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);

	pDc = &priv->dc;
	pDc->priv = priv;

	/* read out the acx100 physical start address for the queues */
	if (!acx100_interrogate(priv, &MemMap, ACX100_RID_MEMORY_MAP)) {
		acxlog(L_BINSTD, "ctlMemoryMapRead returns error\n");
		FN_EXIT(1, 2);
		return 2;
	}

	/* # of items in Rx and Tx queues */
	priv->TxQueueNo = TXBUFFERNO;
	priv->RxQueueNo = RXBUFFERNO;

	/* calculate size of queues */
	qcfg.AreaSize = (sizeof(struct txdescriptor) * TXBUFFERNO +
	                 sizeof(struct rxdescriptor) * RXBUFFERNO + 8);
	qcfg.NumTxQueues = 1;  /* number of tx queues */

	/* sets the beginning of the tx descriptor queue */
	pDc->ui32ACXTxQueueStart = MemMap.QueueStart;
	qcfg.TxQueueStart = pDc->ui32ACXTxQueueStart;
	qcfg.TxQueuePri = 0;

#if (WLAN_HOSTIF==WLAN_USB)
	qcfg.NumTxDesc = TXBUFFERNO;
	qcfg.NumRxDesc = RXBUFFERNO;
#endif

	/* sets the beginning of the rx descriptor queue */
	pDc->ui32ACXRxQueueStart = priv->TxQueueNo * sizeof(struct txdescriptor) + MemMap.QueueStart;
	qcfg.RxQueueStart = pDc->ui32ACXRxQueueStart;
	qcfg.QueueOptions = 1;		/* auto reset descriptor */

	/* sets the end of the rx descriptor queue */
	qcfg.QueueEnd = priv->RxQueueNo * sizeof(struct rxdescriptor) + pDc->ui32ACXRxQueueStart;

	/* sets the beginning of the next queue */
	qcfg.HostQueueEnd = qcfg.QueueEnd + 8;

	acxlog(L_BINDEBUG, "<== Initialize the Queue Indicator\n");

	if (!acx100_configure_length(priv, &qcfg, ACX100_RID_QUEUE_CONFIG, sizeof(QueueConfig_t)-4)){ //0x14 + (qcfg.vale * 8))) {
		acxlog(L_BINSTD, "ctlQueueConfigurationWrite returns error\n");
		goto error;
	}

	if (acx100_create_tx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_tx_host_desc_queue returns error\n");
		goto error;
	}
	if (acx100_create_rx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_rx_host_desc_queue returns error\n");
		goto error;
	}
	acx100_create_tx_desc_queue(pDc);
	acx100_create_rx_desc_queue(pDc);
	if (!acx100_interrogate(priv, &MemMap, ACX100_RID_MEMORY_MAP)) {
		acxlog(L_BINSTD, "Failed to read memory map\n");
		goto error;
	}

#if (WLAN_HOSTIF==WLAN_USB)
	if (!acx100_configure(priv,&MemMap,ACX100_RID_MEMORY_MAP)) {
		acxlog(L_BINSTD,"Failed to write memory map\n");
		goto error;
	}
#endif

	MemMap.PoolStart = (MemMap.QueueEnd + 0x1F + 4) & 0xffffffe0;

	if (!acx100_configure(priv, &MemMap, ACX100_RID_MEMORY_MAP)) {
		acxlog(L_BINSTD, "ctlMemoryMapWrite returns error\n");
		goto error;
	}

	if (!acx100_init_memory_pools(priv, (acx100_memmap_t *) &MemMap)) {
		acxlog(L_BINSTD, "acx100_init_memory_pools returns error\n");
		goto error;
	}

#if THIS_IS_BOGUS_ISNT_IT
	acx100_set_defaults(priv);
#endif

	FN_EXIT(1, 0);
	return 0;

error:
	acx100_free_desc_queues(pDc);

	FN_EXIT(1, 1);
	return 1;
}




int acx111_create_dma_regions(wlandevice_t *priv)
{


	struct ACX111_RX_CONFIG_BLOCK;

	/* TODO make a cool struct and place it in the wlandev struct ? */
	/* This struct is specific to the ACX111 !!! */
	struct ACX111MemoryConfiguration {

		UINT16 id;
		UINT16 length;
		UINT16 no_of_stations;
		UINT16 memory_block_size;
		UINT8 tx_rx_memory_block_allocation;
		UINT8 count_rx_queues;
		UINT8 count_tx_queues;
		UINT8 options;
		UINT8 fragmentation;
		UINT16 reserved1;
		UINT8 reserved2;

		/* start of rx1 block */
		UINT8 rx_queue1_count_descs;
		UINT8 rx_queue1_reserved1;
		UINT8 rx_queue1_reserved2; /* must be set to 7 */
		UINT8 rx_queue1_reserved3; /* must be set to 0 */
		UINT32 rx_queue1_host_rx_start;
		/* end of rx1 block */

		/* start of tx1 block */
		UINT8 tx_queue1_count_descs;
		UINT8 tx_queue1_reserved1;
		UINT8 tx_queue1_reserved2;
		UINT8 tx_queue1_attributes;
		/* end of tx1 block */

#ifdef COMMENT
		/* start init struct */
		pt->m.gp.bytes[0x00] = 0x3; /* id */
		pt->m.gp.bytes[0x01] = 0x0; /* id */
		pt->m.gp.bytes[0x02] = 16; /* length */
		pt->m.gp.bytes[0x03] = 0; /* length */
		pt->m.gp.bytes[0x04] = 0; /* number of sta's */
		pt->m.gp.bytes[0x05] = 0; /* number of sta's */
		pt->m.gp.bytes[0x06] = 0x00; /* memory block size */
		pt->m.gp.bytes[0x07] = 0x01; /* memory block size */
		pt->m.gp.bytes[0x08] = 10; /* tx/rx memory block allocation */
		pt->m.gp.bytes[0x09] = 0; /* number of Rx Descriptor Queues */
		pt->m.gp.bytes[0x0a] = 0; /* number of Tx Descriptor Queues */
		pt->m.gp.bytes[0x0b] = 0; /* options */
		pt->m.gp.bytes[0x0c] = 0x0c; /* Tx memory/fragment memory pool allocation */
		pt->m.gp.bytes[0x0d] = 0; /* reserved */
		pt->m.gp.bytes[0x0e] = 0; /* reserved */
		pt->m.gp.bytes[0x0f] = 0; /* reserved */
		/* end init struct */
#endif 

	}  __WLAN_ATTRIB_PACK__ memconf;

	/* FIXME: way too big to reside on the stack */
	struct {
		UINT16 val0x0[14];
		UINT queue_start;
		UINT val0x20;
		UINT val0x24;
		UINT val0x28;
		UINT val0x2c;
	} MemMap;
	struct TIWLAN_DC *pDc;
	QueueConfig_t qcfg;

	FN_ENTER;

	pDc = &priv->dc;
	pDc->priv = priv;

	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);

	/* FIXME: which memmap is read here, acx100_init_memory_pools did not get called yet ? */
	/* read out the acx100 physical start address for the queues */
	if (!acx100_interrogate(priv, &MemMap, ACX100_RID_MEMORY_MAP)) {
		acxlog(L_BINSTD, "ctlMemoryMapRead returns error\n");
		FN_EXIT(1, 2);
		return 2;
	}

	/* ### Calculate memory positions and queue sizes #### */

	/* calculate size of queues */
	qcfg.AreaSize = (sizeof(struct txdescriptor) * TXBUFFERNO +
	                 sizeof(struct rxdescriptor) * RXBUFFERNO + 8);
	qcfg.NumTxQueues = 1;		/* number of tx queues */
	
	qcfg.NumRxDesc = RXBUFFERNO;
	qcfg.NumTxDesc = TXBUFFERNO;

	/* sets the beginning of the tx descriptor queue */
	pDc->ui32ACXTxQueueStart = MemMap.queue_start;
	qcfg.TxQueueStart = pDc->ui32ACXTxQueueStart;
	qcfg.TxQueuePri = 0;  		/* highest prioriity  ( lowest = 0x7f ) */

	/* sets the beginning of the rx descriptor queue */
	pDc->ui32ACXRxQueueStart = priv->TxQueueNo * sizeof(struct txdescriptor) + MemMap.queue_start;
	qcfg.RxQueueStart = pDc->ui32ACXRxQueueStart;
	qcfg.QueueOptions = 1;

	/* sets the end of the rx descriptor queue */
	qcfg.QueueEnd = priv->RxQueueNo * sizeof(struct rxdescriptor) + pDc->ui32ACXRxQueueStart;

	/* sets the beginning of the next queue */
	qcfg.HostQueueEnd = qcfg.QueueEnd + 8;

	acxlog(L_BINDEBUG, "<== Initialize the Queue Indicator\n");


	/* ### Set up the card #### */

	memset(&memconf, 0, sizeof(memconf));

	/* set command (ACXMemoryConfiguration) */
	memconf.id = 0x03; 
	memconf.length = sizeof(memconf);

	/* hm, I hope this is correct */
	memconf.no_of_stations = 1;

	/* specify the memory block size. Default is 256 */
	memconf.memory_block_size = priv->memblocksize; 	

	/* let's use 50%/50% for tx/rx */
	memconf.tx_rx_memory_block_allocation = 10;


	/* set the count of our queues */
	memconf.count_rx_queues = 1; // TODO place this in constants
	memconf.count_tx_queues = 1;

	if(memconf.count_rx_queues != 1 || memconf.count_tx_queues != 1) {
		acxlog(L_STD, 
			"%s: Requested more buffers than supported.  Please adapt the structure! rxbuffers:%d txbuffers:%d\n", 
			__func__, memconf.count_rx_queues, memconf.count_tx_queues);
		goto error;
	}
	priv->TxQueueNo = TXBUFFERNO;
	priv->RxQueueNo = RXBUFFERNO;

	/* uhoh, hope this is correct-> BusMaster Indirect Memory Organization */
	memconf.options = 1;

	/* let's use 25% for fragmentations and 75% for frame transfers */
	memconf.fragmentation = 0x0f; 

	/* RX queue config */
	memconf.rx_queue1_count_descs = RXBUFFERNO;
	memconf.rx_queue1_reserved2 = 7; /* must be set to 7 */
	memconf.rx_queue1_host_rx_start = qcfg.RxQueueStart;

	/* TX queue config */
	memconf.tx_queue1_count_descs = TXBUFFERNO;
	/* memconf.tx_queue1_host_tx_start = qcfg.TxQueueStart; */


	if (acx100_create_tx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_tx_host_desc_queue returns error\n");
		goto error;
	}
	if (acx100_create_rx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_rx_host_desc_queue returns error\n");
		goto error;
	}
	acx100_create_tx_desc_queue(pDc);
	acx100_create_rx_desc_queue(pDc);

	acxlog(L_STD, "%s: set up acx111 queue memory configuration (queue configs + descriptors)\n", __func__);
	if (acx100_configure(priv, &memconf, 0x03) == 0) {
		acxlog(L_STD, "setting up the memory configuration failed!\n");
		goto error;
	}


#if THIS_IS_BOGUS_ISNT_IT
	acx100_set_defaults(priv);
#endif

	FN_EXIT(1, 0);
	return 0;

error:
	acx100_free_desc_queues(pDc);

	FN_EXIT(1, 1);
	return 1;
}





/*----------------------------------------------------------------
* acx100_delete_dma_region
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_delete_dma_region(wlandevice_t *priv)
{
	FN_ENTER;
#if (WLAN_HOSTIF!=WLAN_USB)
	acx100_write_reg16(priv, priv->io[IO_ACX_ENABLE], 0);

	/* used to be a for loop 1000, do scheduled delay instead */
	acx100_schedule(HZ / 10);
#endif
	acx100_free_desc_queues(&priv->dc);

	FN_EXIT(0, 0);
	return 0;
}

/*----------------------------------------------------------------
* acx100_dma_tx_data
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_dma_tx_data(wlandevice_t *priv, struct txdescriptor *tx_desc)
{
	struct txhostdescriptor *header;
	struct txhostdescriptor *payload;
	unsigned long flags;
	int i;

	FN_ENTER;
	/* header and payload are located in adjacent descriptors */
	header = tx_desc->host_desc;
	payload = tx_desc->host_desc + 1;

	tx_desc->tx_time = jiffies;

	if (1 == priv->preamble_flag)
		tx_desc->Ctl |= ACX100_CTL_PREAMBLE; /* set Preamble */
	/* It seems as if the Preamble setting was actually REVERSED:
	 * bit 0 should most likely actually be ACTIVATED
	 * for Short Preamble, not the other way around as before!
	 * This caused many Tx error 0x20 errors with APs
	 * that don't support Long Preamble, since we were
	 * thinking we are setting Long Preamble, when in fact
	 * it was Short Preamble.
	 * The flag reversal theory has been sort of confirmed
	 * by throughput measurements:
	 * ~ 680K/s with flag disabled
	 * ~ 760K/s with flag enabled
	 */

	/* set autodma and reclaim and 1st mpdu */
	tx_desc->Ctl |= ACX100_CTL_AUTODMA | ACX100_CTL_RECLAIM | ACX100_CTL_FIRSTFRAG;

	/* let chip do RTS/CTS handshaking before sending
	 * in case packet size exceeds threshold */
	if (tx_desc->total_length > priv->rts_threshold)
		tx_desc->Ctl2 |= DESC_CTL2_RTS;

	/* set rate */
	if (WLAN_GET_FC_FTYPE(((p80211_hdr_t*)header->data)->a3.fc) == WLAN_FTYPE_MGMT) {
		tx_desc->rate = 20;	/* 2Mbps for MGMT pkt compatibility */
	} else {
		tx_desc->rate = priv->bitrateval;
	}

	acxlog(L_XFER | L_DATA,
		"Tx pkt (%s): len %i, hdr_len %i, pyld_len %i, mode %d, status %d\n",
		acx100_get_packet_type_string(((p80211_hdr_t*)header->data)->a3.fc),
		tx_desc->total_length,
		header->length,
		payload->length,
		priv->mode,
		priv->status);

	acxlog(L_DATA, "802.11 header[%d]: ", header->length);
#if (WLAN_HOSTIF==WLAN_ISB)
	if (debug&L_DATA) acx100usb_dump_bytes(header->data,header->length);
#else
	for (i = 0; i < header->length; i++)
		acxlog(L_DATA, "%02x ", ((UINT8 *) header->data)[i]);
	acxlog(L_DATA, "\n");
#endif
	acxlog(L_DATA, "802.11 payload[%d]: ", payload->length);
#if (WLAN_HOSTIF==WLAN_USB)
	if (debug&L_DATA) acx100usb_dump_bytes(payload->data,payload->length);
#else
	for (i = 0; i < payload->length; i++)
		acxlog(L_DATA, "%02x ", ((UINT8 *) payload->data)[i]);
	acxlog(L_DATA, "\n");
#endif
#if (WLAN_HOSTIF!=WLAN_USB)
	spin_lock_irqsave(&tx_lock, flags);

	/* sets Ctl ACX100_CTL_OWN to zero telling that the descriptors are now owned by the acx100 */
	header->Ctl &= (UINT16) ~ACX100_CTL_OWN;
	payload->Ctl &= (UINT16) ~ACX100_CTL_OWN;
	tx_desc->Ctl &= (UINT16) ~ACX100_CTL_OWN;

	acx100_write_reg16(priv, priv->io[IO_ACX_INT_TRIG], 0x4);

	spin_unlock_irqrestore(&tx_lock, flags);
#else
	acx100usb_tx_data(priv,tx_desc);
#endif
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_log_txbuffer
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_log_txbuffer(TIWLAN_DC *pDc)
{
	unsigned int i;
	txdesc_t *pTxDesc;

	FN_ENTER;
	for (i = 0; i < pDc->tx_pool_count; i++)
	{
		pTxDesc = &pDc->pTxDescQPool[i];

		if ((pTxDesc->Ctl & DESC_CTL_DONE) == DESC_CTL_DONE)
			acxlog(L_BUF, "txbuf %d done\n", i);
	}
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_clean_tx_desc
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*	This function probably resets the txdescs' status when the ACX100
*	signals the TX done IRQ (txdescs have been processed), starting with
*	the pool index of the descriptor which we would use next,
*	in order to make sure that we can be as fast as possible
*	in filling new txdescs.
*	Oops, now we have our own index, so everytime we get called we know
*	where the next packet to be cleaned is.
*	Hmm, still need to loop through the whole ring buffer now,
*	since we lost sync for some reason when ping flooding or so...
*	(somehow we don't get the IRQ for acx100_clean_tx_desc any more when
*	too many packets are being sent!)
*	FIXME: currently we only process one packet, but this gets out of
*	sync for some reason when ping flooding, so we need to loop,
*	but the previous smart loop implementation causes the ping latency
*	to rise dramatically (~3000 ms), at least on CardBus PheeNet WL-0022.
*	Dunno what to do :-\
*
*----------------------------------------------------------------*/

void acx100_clean_tx_desc(wlandevice_t *priv)
{
	TIWLAN_DC *pDc = &priv->dc;
	txdesc_t *pTxDesc;
	UINT finger, watch;
	unsigned long flags;

	FN_ENTER;

	acx100_log_txbuffer(pDc);
	acxlog(L_BUF, "cleaning up Tx bufs from %ld\n", pDc->tx_tail);

	spin_lock_irqsave(&tx_lock, flags);

	finger = pDc->tx_tail;
	watch = finger;

	do {
		pTxDesc = &pDc->pTxDescQPool[finger];

		/* check if txdesc is marked as "Tx finished" and "owned" */
		if ((pTxDesc->Ctl & DESC_CTL_DONE) == DESC_CTL_DONE) {

			acxlog(L_BUF, "cleaning %d\n", finger);

			if (pTxDesc->error != 0) {
				char *err;

				switch(pTxDesc->error) {
					case 0x10:
						err = "MSDU lifetime timeout? - change 'iwconfig retry lifetime XXX'";
						break;
					case 0x20:
						err = "maybe distance too high? - change 'iwconfig txpower XXX'";
						break;
					default:
						err = "unknown error";
						break;
				}
				acxlog(L_STD, "Tx error occurred (error 0x%02X)!! (%s)\n", pTxDesc->error, err);
				priv->stats.tx_carrier_errors++;
				priv->stats.tx_errors++;
			}

			/* free it */
			pTxDesc->Ctl = ACX100_CTL_OWN;
			
			priv->TxQueueFree++;

			if ((priv->TxQueueFree >= MINFREE_TX + 3)
			&& (priv->status == ISTATUS_4_ASSOCIATED)
			&& (netif_queue_stopped(priv->netdev)))
			{
				/* FIXME: if construct is ugly:
				 * should have functions acx100_stop_queue
				 * etc. which set flag priv->tx_stopped
				 * to be checked here. */
				acxlog(L_XFER, "wake queue (avail. Tx desc %ld).\n", priv->TxQueueFree);
				netif_wake_queue(priv->netdev);
			}
		}
		else
			break;

		/* update pointer for descr to be cleaned next */
		finger = (finger + 1) % pDc->tx_pool_count;
	} while (watch != finger);

	/* remember last position */
	pDc->tx_tail = finger;

	spin_unlock_irqrestore(&tx_lock, flags);

	FN_EXIT(0, 0);
	return;
}
/*----------------------------------------------------------------
* acx100_rxmonitor
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_rxmonitor(wlandevice_t *priv, struct rxbuffer *buf)
{
	unsigned long *data = (unsigned long *)buf;
	unsigned int packet_len = data[0] & 0xFFF;
	int sig_strength = (data[1] >> 16) & 0xFF;
	int sig_quality  = (data[1] >> 24) & 0xFF;
	p80211msg_lnxind_wlansniffrm_t *msg;

	int payload_offset = 0;
	unsigned int skb_len;
	struct sk_buff *skb;
	void *datap;

	FN_ENTER;

	if (!(priv->rx_config_1 & RX_CFG1_PLUS_ADDIT_HDR))
	{
		printk("rx_config_1 misses RX_CFG1_PLUS_ADDIT_HDR\n");
		FN_EXIT(0, 0);
		return;
	}

	if (priv->rx_config_1 & RX_CFG1_PLUS_ADDIT_HDR)
	{
		payload_offset += 3*4; /* status words         */
		packet_len     += 3*4; /* length is w/o status */
	}
		
	if (priv->rx_config_1 & RX_CFG1_INCLUDE_ADDIT_HDR)
		payload_offset += 4;   /* phy header   */

	/* we are in big luck: the acx100 doesn't modify any of the fields */
	/* in the 802.11-frame. just pass this packet into the PF_PACKET-  */
	/* subsystem. yeah. */

	skb_len = packet_len - payload_offset;

	if (priv->netdev->type == ARPHRD_IEEE80211_PRISM)
		skb_len += sizeof(p80211msg_lnxind_wlansniffrm_t);

		/* sanity check */
	if (skb_len > (WLAN_HDR_A4_LEN + WLAN_DATA_MAXLEN + WLAN_CRC_LEN))
	{
		printk("monitor mode panic: oversized frame!\n");
		FN_EXIT(0, 0);
		return;
	}

		/* allocate skb */
	if ( (skb = dev_alloc_skb(skb_len)) == NULL)
	{
		printk("alloc_skb failed trying to allocate %d bytes\n", skb_len);
		FN_EXIT(0, 0);
		return;
	}

	skb_put(skb, skb_len);

		/* when in raw 802.11 mode, just copy frame as-is */
	if (priv->netdev->type == ARPHRD_IEEE80211)
		datap = skb->data;
	else  /* otherwise, emulate prism header */
	{
		msg = (p80211msg_lnxind_wlansniffrm_t*)skb->data;
		datap = msg + 1;
		
		msg->msgcode = DIDmsg_lnxind_wlansniffrm;
		msg->msglen = sizeof(p80211msg_lnxind_wlansniffrm_t);
		strcpy(msg->devname, priv->netdev->name);
		
		msg->hosttime.did = DIDmsg_lnxind_wlansniffrm_hosttime;
		msg->hosttime.status = 0;
		msg->hosttime.len = 4;
		msg->hosttime.data = jiffies;
		
		msg->mactime.did = DIDmsg_lnxind_wlansniffrm_mactime;
		msg->mactime.status = 0;
		msg->mactime.len = 4;
		msg->mactime.data = data[2];
		
		msg->channel.did = DIDmsg_lnxind_wlansniffrm_channel;
		msg->channel.status = P80211ENUM_msgitem_status_no_value;
		msg->channel.len = 4;
		msg->channel.data = 0;

		msg->rssi.did = DIDmsg_lnxind_wlansniffrm_rssi;
		msg->rssi.status = P80211ENUM_msgitem_status_no_value;
		msg->rssi.len = 4;
		msg->rssi.data = 0;

		msg->sq.did = DIDmsg_lnxind_wlansniffrm_sq;
		msg->sq.status = P80211ENUM_msgitem_status_no_value;
		msg->sq.len = 4;
		msg->sq.data = 0;

		msg->signal.did = DIDmsg_lnxind_wlansniffrm_signal;
		msg->signal.status = 0;
		msg->signal.len = 4;
		msg->signal.data = sig_quality;

		msg->noise.did = DIDmsg_lnxind_wlansniffrm_noise;
		msg->noise.status = 0;
		msg->noise.len = 4;
		msg->noise.data = sig_strength;

		msg->rate.did = DIDmsg_lnxind_wlansniffrm_rate;
		msg->rate.status = P80211ENUM_msgitem_status_no_value;
		msg->rate.len = 4;
		msg->rate.data = 0;

		msg->istx.did = DIDmsg_lnxind_wlansniffrm_istx;
		msg->istx.status = 0;
		msg->istx.len = 4;
		msg->istx.data = P80211ENUM_truth_false;

		skb_len -= sizeof(p80211msg_lnxind_wlansniffrm_t);

		msg->frmlen.did = DIDmsg_lnxind_wlansniffrm_signal;
		msg->frmlen.status = 0;
		msg->frmlen.len = 4;
		msg->frmlen.data = skb_len;
	}

	memcpy(datap, ((unsigned char*)buf)+payload_offset, skb_len);

	skb->dev = priv->netdev;
	skb->dev->last_rx = jiffies;

	skb->mac.raw = skb->data;
	skb->ip_summed = CHECKSUM_NONE;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_80211_RAW);

	priv->stats.rx_packets++;
	priv->stats.rx_bytes += skb->len;

	netif_rx(skb);
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_log_rxbuffer
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_log_rxbuffer(TIWLAN_DC *pDc)
{
	unsigned int i;
	struct rxhostdescriptor *pDesc;

	FN_ENTER;
	for (i = 0; i < pDc->rx_pool_count; i++)
	{
		pDesc = &pDc->pRxHostDescQPool[i];
#if (WLAN_HOSTIF==WLAN_USB)
		acxlog(L_DEBUG,"rxbuf %d Ctl=%X  val0x14=%X\n",i,pDesc->Ctl,pDesc->Status);
#endif
		if ((pDesc->Ctl & ACX100_CTL_OWN) && (pDesc->Status & BIT31))
			acxlog(L_BUF, "rxbuf %d full\n", i);
	}
	FN_EXIT(0, 0);
}

/*------------------------------------------------------------------------------
 * acx100_process_rx_desc
 *
 * RX path top level entry point called directly and only from the IRQ handler
 *
 * Arguments:
 *
 * Returns:
 *
 * Side effects:
 *
 * Call context: Hard IRQ
 *
 * STATUS:
 *
 * Comment:
 *
 *----------------------------------------------------------------------------*/
void acx100_process_rx_desc(wlandevice_t *priv)
{
	struct rxhostdescriptor *RxPool;
	TIWLAN_DC *pDc;
	struct rxhostdescriptor *pDesc;
	UINT16 buf_len;
	unsigned long flags;
	int curr_idx;
	unsigned int count = 0;
	p80211_hdr_t *buf;

	FN_ENTER;

	pDc = &priv->dc;
	acx100_log_rxbuffer(pDc);

	/* there used to be entry count code here, but because this function is called
	 * by the interrupt handler, we are sure that this will only be entered once
	 * because the kernel locks the interrupt handler */

	RxPool = pDc->pRxHostDescQPool;

	/* First, have a loop to determine the first descriptor that's
	 * full, just in case there's a mismatch between our current
	 * rx_tail and the full descriptor we're supposed to handle. */
	spin_lock_irqsave(&rx_lock, flags);
	do {
		count++;
		if (count > pDc->rx_pool_count)
		{ /* hmm, no luck: all descriptors empty, bail out */
			spin_unlock_irqrestore(&rx_lock, flags);
			FN_EXIT(0, 0);
			return;
		}
		curr_idx = pDc->rx_tail;
		pDesc = &RxPool[pDc->rx_tail];
		pDc->rx_tail = (pDc->rx_tail + 1) % pDc->rx_pool_count;
	} while (!((pDesc->Ctl & ACX100_CTL_OWN) && (pDesc->Status & BIT31))); 
	/* "pDesc->val0x14 < 0" is there to check whether MSB is set or not */
	/* check whether descriptor full, advance to next one if not */
	spin_unlock_irqrestore(&rx_lock, flags);

	while (1)
	{
		acxlog(L_BUF, "%s: using curr_idx %d, rx_tail is now %ld\n", __func__, curr_idx, pDc->rx_tail);

		buf = (p80211_hdr_t *)&pDesc->data->buf;
		if (priv->rx_config_1 & RX_CFG1_INCLUDE_ADDIT_HDR) {
			/* take into account additional header in front of packet */
			buf = (p80211_hdr_t*)((UINT8*)buf + 4);
		}

		buf_len = pDesc->data->status & 0xfff;      /* somelength */
		if ((WLAN_GET_FC_FSTYPE(buf->a3.fc) != WLAN_FSTYPE_BEACON)
		||  (debug & L_XFER_BEACON))
			acxlog(L_XFER|L_DATA, "Rx pkt %02d (%s): time %lu, len %i, signal %d, SNR %d, mode %d, status %d\n",
				curr_idx,
				acx100_get_packet_type_string(buf->a3.fc),
				pDesc->data->time,
				buf_len,
				pDesc->data->level,
				pDesc->data->snr,
				priv->mode,
				priv->status);

		/* I tried to figure out how to map these levels to dBm
		 * values, but for the life of me I really didn't
		 * manage to get it. Either these values are not meant to
		 * be expressed in dBm, or it's some pretty complicated
		 * calculation. */
		priv->wstats.qual.level = pDesc->data->level * 100 / 255;
		priv->wstats.qual.noise = pDesc->data->snr * 100 / 255;
		priv->wstats.qual.qual =
			(priv->wstats.qual.noise <= 100) ?
			      100 - priv->wstats.qual.noise : 0;
		priv->wstats.qual.updated = 7;

		if (priv->monitor) {
			acx100_rxmonitor(priv, pDesc->data);
		} else if (buf_len >= 14) {
			acx100_rx_ieee802_11_frame(priv, pDesc);
		} else {
			acxlog(L_DEBUG | L_XFER | L_DATA,
			       "NOT receiving packet (%s): size too small (%d)\n",
			       acx100_get_packet_type_string(buf->a3.fc), buf_len);
		}

		pDesc->Ctl &= ~ACX100_CTL_OWN; /* Host no longer owns this */
		pDesc->Status = 0;

		/* ok, descriptor is handled, now check the next descriptor */
		spin_lock_irqsave(&rx_lock, flags);
		curr_idx = pDc->rx_tail;
		pDesc = &RxPool[pDc->rx_tail];

		/* if next descriptor is empty, then bail out */
		if (!((pDesc->Ctl & ACX100_CTL_OWN) && (pDesc->Status & BIT31)))
		{
			spin_unlock_irqrestore(&rx_lock, flags);
			break;
		}
		else
		{
			pDc->rx_tail = (pDc->rx_tail + 1) % pDc->rx_pool_count;
			spin_unlock_irqrestore(&rx_lock, flags);
		}
	}
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_create_tx_host_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_create_tx_host_desc_queue(TIWLAN_DC * pDc)
{
	wlandevice_t *priv;

	UINT i;
	UINT align_offs;
	UINT alignment;

	struct framehdr *frame_hdr;
	struct framehdr *frame_hdr_phy;

	UINT8 *frame_payload;
	UINT8 *frame_payload_phy;

	struct txhostdescriptor *host_desc;
	struct txhostdescriptor *host_desc_phy;

	FN_ENTER;

	priv = pDc->priv;

	/* allocate TX header pool */
	pDc->FrameHdrQPoolSize = (priv->TxQueueNo * sizeof(struct framehdr));
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pFrameHdrQPool =
	      pci_alloc_consistent(0, pDc->FrameHdrQPoolSize, &pDc->FrameHdrQPoolPhyAddr))) {
		acxlog(L_BINSTD, "pDc->pFrameHdrQPool memory allocation error\n");
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->pFrameHdrQPool = 0x%8x\n", (UINT) pDc->pFrameHdrQPool);
	acxlog(L_BINDEBUG, "pDc->pFrameHdrQPoolPhyAddr = 0x%8x\n", (UINT) pDc->FrameHdrQPoolPhyAddr);
#else
	if ((pDc->pFrameHdrQPool=kmalloc(pDc->FrameHdrQPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"pDc->pFrameHdrQPool memory allocation error\n");
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pFrameHdrQPool,0,pDc->FrameHdrQPoolSize);
#endif

	/* allocate TX payload pool */
	pDc->TxBufferPoolSize = priv->TxQueueNo*2 * (WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN);
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pTxBufferPool =
	      pci_alloc_consistent(0, pDc->TxBufferPoolSize, &pDc->TxBufferPoolPhyAddr))) {
		acxlog(L_BINSTD, "pDc->pTxBufferPool memory allocation error\n");
		pci_free_consistent(0, pDc->FrameHdrQPoolSize,
				    pDc->pFrameHdrQPool,
				    pDc->FrameHdrQPoolPhyAddr);
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->TxBufferPool = 0x%8x\n", (UINT) pDc->pTxBufferPool);
	acxlog(L_BINDEBUG, "pDc->TxBufferPoolPhyAddr = 0x%8x\n", (UINT) pDc->TxBufferPoolPhyAddr);
#else
	if ((pDc->pTxBufferPool=kmalloc(pDc->TxBufferPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"pDc->pTxBufferPool memory allocation error\n");
		kfree(pDc->pFrameHdrQPool);
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pTxBufferPool,0,pDc->TxBufferPoolSize);
#endif

	/* allocate the TX host descriptor queue pool */
	pDc->TxHostDescQPoolSize =  priv->TxQueueNo*2 * sizeof(struct txhostdescriptor) + 3;
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pTxHostDescQPool =
	      pci_alloc_consistent(0, pDc->TxHostDescQPoolSize,
				  (dma_addr_t *) &pDc->TxHostDescQPoolPhyAddr))) {
		acxlog(L_BINSTD, "Failed to allocate shared memory for TxHostDesc queue\n");
		pci_free_consistent(0, pDc->FrameHdrQPoolSize,
				    pDc->pFrameHdrQPool,
				    pDc->FrameHdrQPoolPhyAddr);
		pci_free_consistent(0, pDc->TxBufferPoolSize,
				    pDc->pTxBufferPool,
				    pDc->TxBufferPoolPhyAddr);
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->pTxHostDescQPool = 0x%8x\n", (UINT) pDc->pTxHostDescQPool);
	acxlog(L_BINDEBUG, "pDc->TxHostDescQPoolPhyAddr = 0x%8lx\n", pDc->TxHostDescQPoolPhyAddr);
#else
	if ((pDc->pTxHostDescQPool=kmalloc(pDc->TxHostDescQPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"Failed to allocate memory for TxHostDesc queue\n");
		kfree(pDc->pFrameHdrQPool);
		kfree(pDc->pTxBufferPool);
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pTxHostDescQPool,0,pDc->TxHostDescQPoolSize);
#endif

#if (WLAN_HOSTIF!=WLAN_USB)
	/* check for proper alignment of TX host descriptor pool */
	alignment = (UINT) pDc->pTxHostDescQPool & 3;
	if (alignment) {
		acxlog(L_BINSTD, "%s: TxHostDescQPool not aligned properly\n", __func__);
		align_offs = 4 - alignment;
	} else {
		align_offs = 0;
	}

	host_desc = (struct txhostdescriptor *) ((UINT8 *) pDc->pTxHostDescQPool + align_offs);
	host_desc_phy = (struct txhostdescriptor *) ((UINT8 *) pDc->TxHostDescQPoolPhyAddr + align_offs);
	frame_hdr_phy = (struct framehdr *) pDc->FrameHdrQPoolPhyAddr;
	frame_payload_phy = (UINT8 *) pDc->TxBufferPoolPhyAddr;
#else
	host_desc=(struct txhostdescriptor *)pDc->pTxHostDescQPool;
#endif
	frame_hdr = (struct framehdr *) pDc->pFrameHdrQPool;
	frame_payload = (UINT8 *) pDc->pTxBufferPool;

	for (i = 0; i < priv->TxQueueNo*2 - 1; i++)
	{
		if (!(i & 1)) {
#if (WLAN_HOSTIF!=WLAN_USB)
			host_desc->data_phy = (UINT8 *) frame_hdr_phy;
			frame_hdr_phy++;
			host_desc->pNext = (struct txhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct txhostdescriptor));
#endif
			host_desc->data = (UINT8 *) frame_hdr;
			frame_hdr++;
		} else {
#if (WLAN_HOSTIF!=WLAN_USB)
			host_desc->data_phy = (UINT8 *) frame_payload_phy;
			frame_payload_phy += WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN;
#endif
			host_desc->data = (UINT8 *) frame_payload;
			frame_payload += WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN;
			host_desc->pNext = NULL;
		}

		host_desc->Ctl |= ACX100_CTL_OWN;
#if (WLAN_HOSTIF!=WLAN_USB)
		host_desc->desc_phy = host_desc_phy;
		host_desc->desc_phy_next = (struct txhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct txhostdescriptor));
		host_desc_phy++;
#endif
		host_desc++;
	}
	host_desc->data = (UINT8 *) frame_payload;
	host_desc->pNext = 0;
	host_desc->Ctl |= ACX100_CTL_OWN;

#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->data_phy = (UINT8 *) frame_payload_phy;
	host_desc->desc_phy = host_desc_phy;
	host_desc->desc_phy_next = (struct txhostdescriptor *)((UINT8 *) pDc->TxHostDescQPoolPhyAddr + align_offs);
#endif

	FN_EXIT(0, 0);
	return 0;
}

/*----------------------------------------------------------------
* acx100_create_rx_host_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_create_rx_host_desc_queue(TIWLAN_DC * pDc)
{
	wlandevice_t *priv;

	UINT i;
	UINT align_offs;
	UINT alignment;

	struct rxbuffer *data;
	struct rxbuffer *data_phy;
	struct rxhostdescriptor *host_desc;
	struct rxhostdescriptor *host_desc_phy;

	int result = 0;

	FN_ENTER;

	priv = pDc->priv;

	/* allocate the RX host descriptor queue pool */
	pDc->RxHostDescQPoolSize = (priv->RxQueueNo * sizeof(struct rxhostdescriptor)) + 0x3;

#if (WLAN_HOSTIF!=WLAN_USB)
	if ((pDc->pRxHostDescQPool =
	     pci_alloc_consistent(0, pDc->RxHostDescQPoolSize,
				(dma_addr_t *)  &pDc->RxHostDescQPoolPhyAddr)) == NULL) {
		acxlog(L_BINSTD,
		       "Failed to allocate shared memory for RxHostDesc queue\n");
		result = 2;
		goto fail;
	}
#else
	if ((pDc->pRxHostDescQPool = kmalloc(pDc->RxHostDescQPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"Failed to allocate memory for RxHostDesc queue\n");
		result=2;
		goto fail;
	}
	memset(pDc->pRxHostDescQPool,0,pDc->RxHostDescQPoolSize);
#endif

	/* allocate RX buffer pool */
	pDc->RxBufferPoolSize = (priv->RxQueueNo * sizeof(struct rxbuffer));
#if (WLAN_HOSTIF!=WLAN_USB)
	if ((pDc->pRxBufferPool =
	     pci_alloc_consistent(0, pDc->RxBufferPoolSize,
				  (dma_addr_t *) &pDc->RxBufferPoolPhyAddr)) == NULL) {
		acxlog(L_BINSTD, "Failed to allocate shared memory for Rx buffer\n");
		pci_free_consistent(0, pDc->RxHostDescQPoolSize,
				    pDc->pRxHostDescQPool,
				    pDc->RxHostDescQPoolPhyAddr);
		result = 2;
		goto fail;
	}
#else
	if ((pDc->pRxBufferPool = kmalloc(pDc->RxBufferPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"Failed to allocate memory for Rx buffer\n");
		result=2;
		goto fail;
	}
	memset(pDc->pRxBufferPool,0,pDc->RxBufferPoolSize);
#endif

	acxlog(L_BINDEBUG, "pDc->pRxHostDescQPool = 0x%8x\n", (UINT) pDc->pRxHostDescQPool);
#if (WLAN_HOSTIF!=WLAN_USB)
	acxlog(L_BINDEBUG, "pDc->RxHostDescQPoolPhyAddr = 0x%8x\n", (UINT) pDc->RxHostDescQPoolPhyAddr);
#endif
	acxlog(L_BINDEBUG, "pDc->pRxBufferPool = 0x%8x\n", (UINT) pDc->pRxBufferPool);
#if (WLAN_HOSTIF!=WLAN_USB)
	acxlog(L_BINDEBUG, "pDc->RxBufferPoolPhyAddr = 0x%8x\n", (UINT) pDc->RxBufferPoolPhyAddr);
#endif
#if (WLAN_HOSTIF!=WLAN_USB)
	/* check for proper alignment of RX host descriptor pool */
	if ((alignment = ((UINT) pDc->pRxHostDescQPool) & 3)) {
		acxlog(L_BINSTD, "acx100_create_rx_host_desc_queue: RxHostDescQPool not aligned properly\n");
		align_offs = 4 - alignment;
	} else {
		align_offs = 0;
	}

	host_desc = (struct rxhostdescriptor *) ((UINT8 *) pDc->pRxHostDescQPool + align_offs);
	host_desc_phy = (struct rxhostdescriptor *) ((UINT8 *) pDc->RxHostDescQPoolPhyAddr + align_offs);

	priv->RxHostDescPoolStart = host_desc_phy;
#else
	host_desc = (struct rxhostdescriptor *)pDc->pRxHostDescQPool;
#endif
	data = (struct rxbuffer *) pDc->pRxBufferPool;
#if (WLAN_HOSTIF!=WLAN_USB)
	data_phy = (struct rxbuffer *) pDc->RxBufferPoolPhyAddr;
#endif

	if (priv->RxQueueNo != 1) {
		for (i = 0; i < priv->RxQueueNo - 1; i++) {
			host_desc->data = data;
#if (WLAN_HOSTIF!=WLAN_USB)
			host_desc->data_phy = data_phy;
			data_phy++;
#endif
			host_desc->length = sizeof(struct rxbuffer);
			data++;

			/* FIXME: what do these mean ? */
			host_desc->val0x28 = 2;
			host_desc->Ctl &= ~0x80;
#if (WLAN_HOSTIF!=WLAN_USB)
			host_desc->desc_phy = host_desc_phy;
			host_desc->desc_phy_next = (struct rxhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct rxhostdescriptor));
			host_desc_phy++;
#endif
			host_desc++;
		}
	}
	host_desc->data = data;
#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->data_phy = data_phy;
#endif
	host_desc->length = sizeof(struct rxbuffer);

	host_desc->val0x28 = 2;
	host_desc->Ctl &= 0xff7f;
#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->desc_phy = host_desc_phy;
	host_desc->desc_phy_next = (struct rxhostdescriptor *)((UINT8 *) pDc->RxHostDescQPoolPhyAddr + align_offs);
#endif
	result = 0;
fail:
	FN_EXIT(1, result);
	return result;
}

/*----------------------------------------------------------------
* acx100_create_tx_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_create_tx_desc_queue(TIWLAN_DC * pDc)
{
	wlandevice_t *priv;

	UINT32 mem_offs;
	UINT32 i;

	struct txdescriptor *tx_desc;
	struct txhostdescriptor *tx_hostdesc;

	UINT hostmemptr;

	FN_ENTER;

	priv = pDc->priv;
	pDc->tx_pool_count = priv->TxQueueNo;
#if (WLAN_HOSTIF==WLAN_USB)
  /* allocate memory for TxDescriptors */
	pDc->pTxDescQPool=(struct txdescriptor *)kmalloc(pDc->tx_pool_count*sizeof(struct txdescriptor),GFP_KERNEL);
	if (!pDc->pTxDescQPool) {
		acxlog(L_STD,"Not enough memory to allocate txdescriptor queue\n");
		return;
	}
#endif
#if (WLAN_HOSTIF!=WLAN_USB)
	pDc->pTxDescQPool = (struct txdescriptor *) (priv->iobase2 +
				     pDc->ui32ACXTxQueueStart);

	acxlog(L_BINDEBUG, "priv->iobase2 = 0x%08lx\n", priv->iobase2);
	acxlog(L_BINDEBUG, "pDc->ui32ACXTxQueueStart = 0x%08lx\n",
	       pDc->ui32ACXTxQueueStart);
	acxlog(L_BINDEBUG, "pDc->pTxDescQPool = 0x%08x\n",
	       (UINT) pDc->pTxDescQPool);
#endif
	priv->TxQueueFree = priv->TxQueueNo;
	pDc->tx_head = 0;
	pDc->tx_tail = 0;
	tx_desc = pDc->pTxDescQPool;
#if (WLAN_HOSTIF!=WLAN_USB)
	mem_offs = pDc->ui32ACXTxQueueStart;
	hostmemptr = pDc->TxHostDescQPoolPhyAddr;
#else
	mem_offs=pDc->pTxDescQPool;
#endif
	tx_hostdesc = (struct txhostdescriptor *) pDc->pTxHostDescQPool;

	/* loop over complete send pool */
	for (i = 0; i < pDc->tx_pool_count; i++) {
		memset(tx_desc, 0, sizeof(struct txdescriptor));
		/* pointer to hostdesc memory */
#if (WLAN_HOSTIF!=WLAN_USB)
		tx_desc->HostMemPtr = hostmemptr;
#else
		tx_desc->HostMemPtr=NULL;
#endif
		/* initialise ctl */
		tx_desc->Ctl = DESC_CTL_INIT;
		tx_desc->Ctl2 = 0;
		/* point to next txdesc */
		tx_desc->pNextDesc = mem_offs + sizeof(struct txdescriptor);
		/* pointer to first txhostdesc */
		tx_desc->host_desc = tx_hostdesc;

		/* reserve two (hdr desc and payload desc) */
		tx_hostdesc += 2;
#if (WLAN_HOSTIF!=WLAN_USB)
		hostmemptr += 2 * sizeof(struct txhostdescriptor);
#endif
		/* go to the next */
		mem_offs += sizeof(struct txdescriptor);
		tx_desc++;
	}
	/* go to the last one */
	tx_desc--;
	/* and point to the first making it a ring buffer */
#if (WLAN_HOSTIF!=WLAN_USB)
	tx_desc->pNextDesc = pDc->ui32ACXTxQueueStart;
#else
	tx_desc->pNextDesc = pDc->pTxDescQPool;
#endif
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_create_rx_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_create_rx_desc_queue(TIWLAN_DC * pDc)
{
	wlandevice_t *priv;
	UINT32 mem_offs;
	UINT32 i;
	struct rxdescriptor *rx_desc;

	FN_ENTER;

	priv = pDc->priv;

#if (WLAN_HOSTIF!=WLAN_USB)
	/* FIXME WHY IS IT "TxQueueNo" ?
	 * Probably because Rx pool ptr should be right AFTER Tx pool */
	pDc->pRxDescQPool = (struct rxdescriptor *) ((priv->TxQueueNo * sizeof(struct txdescriptor)) + (UINT8 *) pDc->pTxDescQPool);
#endif
	pDc->rx_pool_count = priv->RxQueueNo;
	pDc->rx_tail = 0;
#if (WLAN_HOSTIF!=WLAN_USB)
	mem_offs = pDc->ui32ACXRxQueueStart;
	rx_desc = (struct rxdescriptor *) pDc->pRxDescQPool;

	/* loop over complete receive pool */
	for (i = 0; i < pDc->rx_pool_count; i++) {
		memset(rx_desc, 0, sizeof(struct rxdescriptor));
		rx_desc->Ctl = 0xc;

		/* point to next rxdesc */
		rx_desc->pNextDesc = mem_offs + sizeof(struct rxdescriptor); // next rxdesc pNextDesc

		/* go to the next */
		mem_offs += sizeof(struct rxdescriptor);
		rx_desc++;
	}
	/* go to the last one */
	rx_desc--;

	/* and point to the first making it a ring buffer */
	rx_desc->pNextDesc = pDc->ui32ACXRxQueueStart;
#endif
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_free_desc_queues
*
*	Releases the queues that have been allocated, the
*	others have been initialised to NULL in acx100.c so this
*	function can be used if only part of the queues where
*	allocated.
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_free_desc_queues(TIWLAN_DC * pDc)
{
	FN_ENTER;
	if (pDc->pRxHostDescQPool) {
		pci_free_consistent(0,
				    pDc->TxHostDescQPoolSize,
				    pDc->pTxHostDescQPool,
				    pDc->TxHostDescQPoolPhyAddr);
		pDc->pTxHostDescQPool = NULL;
		pDc->TxHostDescQPoolSize = 0;
	}
	if (pDc->pFrameHdrQPool) {
		pci_free_consistent(0, pDc->FrameHdrQPoolSize,
				    pDc->pFrameHdrQPool,
				    pDc->FrameHdrQPoolPhyAddr);
		pDc->pFrameHdrQPool = NULL;
		pDc->FrameHdrQPoolSize = 0;
	}
	if (pDc->pTxBufferPool) {
		pci_free_consistent(0, pDc->TxBufferPoolSize,
				    pDc->pTxBufferPool,
				    pDc->TxBufferPoolPhyAddr);
		pDc->pTxBufferPool = NULL;
		pDc->TxBufferPoolSize = 0;
	}

	pDc->pTxDescQPool = NULL;
	pDc->tx_pool_count = 0;

	if (pDc->pRxHostDescQPool) {
		pci_free_consistent(0, pDc->RxHostDescQPoolSize,
				    pDc->pRxHostDescQPool,
				    pDc->RxHostDescQPoolPhyAddr);
		pDc->pRxHostDescQPool = NULL;
		pDc->RxHostDescQPoolSize = 0;
	}
	if (pDc->pRxBufferPool) {
		pci_free_consistent(0, pDc->RxBufferPoolSize,
				    pDc->pRxBufferPool,
				    pDc->RxBufferPoolPhyAddr);
		pDc->pRxBufferPool = NULL;
		pDc->RxBufferPoolSize = 0;
	}

	pDc->pRxDescQPool = NULL;
	pDc->rx_pool_count = 0;
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_init_memory_pools
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*	FIXME: This function still needs a cleanup
*----------------------------------------------------------------*/

int acx100_init_memory_pools(wlandevice_t *priv, acx100_memmap_t * mmt)
{
	UINT32 TotalMemoryBlocks;  // var_40
        memblocksize_t MemoryBlockSize;

        acx100_memconfigoption_t MemoryConfigOption;    // var_2c

	FN_ENTER;


	/* Let's see if we can follow this:
	   first we select our memory block size (which I think is
	   completely arbitrary) */
	MemoryBlockSize.size = priv->memblocksize;

	/* Then we alert the card to our decision of block size */
	if (!acx100_configure(priv, &MemoryBlockSize, ACX100_RID_BLOCK_SIZE)) {
		acxlog(L_BINSTD, "Ctl: MemoryBlockSizeWrite failed\n");
		FN_EXIT(1, 0);
		return 0;
	}

	/* We figure out how many total blocks we can create, using
	   the block size we chose, and the beginning and ending
	   memory pointers. IE. end-start/size */
	TotalMemoryBlocks = (mmt->PoolEnd - mmt->PoolStart) / priv->memblocksize;

	acxlog(L_DEBUG,"TotalMemoryBlocks=%ld (%ld bytes)\n",TotalMemoryBlocks,TotalMemoryBlocks*priv->memblocksize);

	/* This one I have no idea on */
	/* block-transfer=0x20000
	 * indirect descriptors=0x10000
	 */
#if (WLAN_HOSTIF==WLAN_USB)
	MemoryConfigOption.DMA_config = 0x20000;
#else
	MemoryConfigOption.DMA_config = 0x30000;
#endif


	/* Declare start of the Rx host pool */
	MemoryConfigOption.RxHostDesc = priv->RxHostDescPoolStart;

	/* 50% of the allotment of memory blocks go to tx descriptors */
	MemoryConfigOption.TxBlockNum = TotalMemoryBlocks / 2;

	/* and 50% go to the rx descriptors */
	MemoryConfigOption.RxBlockNum = TotalMemoryBlocks - MemoryConfigOption.TxBlockNum;

	/* in this block, we save the information we gleaned from the
	   card into our wlandevice structure; # of tx desc blocks */
	priv->TxBlockNum = MemoryConfigOption.TxBlockNum;

	/* # of rx desc blocks */
	priv->TxBlockNum = MemoryConfigOption.RxBlockNum;

	/* size of the tx and rx descriptor queues */
	priv->TotalTxBlockSize = MemoryConfigOption.TxBlockNum * priv->memblocksize;
	priv->TotalRxBlockSize = MemoryConfigOption.RxBlockNum * priv->memblocksize;


	/* align the tx descriptor queue to an alignment of 0x20 (32 bytes) */
	MemoryConfigOption.rx_mem = (mmt->PoolStart + 0x1f) & 0xffffffe0;

	/* align the rx descriptor queue to units of 0x20 and offset it
	   by the tx descriptor queue */
	MemoryConfigOption.tx_mem =
	    (0x1f + mmt->PoolStart + priv->TotalRxBlockSize) & 0xffffffe0;

	/* alert the device to our decision */
	if (!acx100_configure(priv, &MemoryConfigOption, ACX100_RID_MEMORY_CONFIG_OPTIONS)) {
		acxlog(L_DEBUG,"%s: configure memory config options failed!\n", __func__);
		FN_EXIT(1, 0);
		return 0;
	}

	/* and tell the device to kick it into gear */
	if (!acx100_issue_cmd(priv, ACX100_CMD_INIT_MEMORY, NULL, 0, 5000)) {
		acxlog(L_DEBUG,"%s: init memory failed!\n", __func__);
		FN_EXIT(1, 0);
		return 0;
	}
	FN_EXIT(1, 1);
	return 1;
}

/*----------------------------------------------------------------
* acx100_get_tx_desc
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

struct txdescriptor *acx100_get_tx_desc(wlandevice_t *priv)
{
	struct TIWLAN_DC * pDc = &priv->dc;
	struct txdescriptor *tx_desc;
	unsigned long flags;

	FN_ENTER;

	spin_lock_irqsave(&tx_lock, flags);

	tx_desc = &pDc->pTxDescQPool[pDc->tx_head];

	if ((tx_desc->Ctl & ACX100_CTL_OWN) == 0) {
		/* whoops, descr at current index is not free, so probably
		 * ring buffer already full */
		tx_desc = NULL;
		goto error;
	}

	priv->TxQueueFree--;
	acxlog(L_BUF, "got Tx desc %ld, %ld remain.\n", pDc->tx_head, priv->TxQueueFree);

/*
 * This comment is probably not entirely correct, needs further discussion
 * (restored commented-out code below to fix Tx ring buffer overflow,
 * since it's much better to have a slightly less efficiently used ring
 * buffer rather than one which easily overflows):
 *
 * This doesn't do anything other than limit our maximum number of
 * buffers used at a single time (we might as well just declare
 * MINFREE_TX less descriptors when we open up.) We should just let it
 * slide here, and back off MINFREE_TX in acx100_clean_tx_desc, when given the
 * opportunity to let the queue start back up.
 */
	if (priv->TxQueueFree < MINFREE_TX)
	{
		acxlog(L_XFER, "stop queue (avail. Tx desc %ld).\n", priv->TxQueueFree);
		netif_stop_queue(priv->netdev);
	}

	/* returning current descriptor, so advance to next free one */
	pDc->tx_head = (pDc->tx_head + 1) % pDc->tx_pool_count;
error:
	spin_unlock_irqrestore(&tx_lock, flags);

	FN_EXIT(0, (int)tx_desc);
	return tx_desc;
}

static char type_string[32];	/* I *really* don't care that this is static,
				   so don't complain, else... ;-) */

/*----------------------------------------------------------------
* acx100_get_packet_type_string
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

char *acx100_get_packet_type_string(UINT16 fc)
{
	char *ftype = "UNKNOWN", *fstype = "UNKNOWN";

	FN_ENTER;
	switch (WLAN_GET_FC_FTYPE(fc)) {
	case WLAN_FTYPE_MGMT:
		ftype = "MGMT";
		switch (WLAN_GET_FC_FSTYPE(fc)) {
		case WLAN_FSTYPE_ASSOCREQ:
			fstype = "AssocReq";
			break;
		case WLAN_FSTYPE_ASSOCRESP:
			fstype = "AssocResp";
			break;
		case WLAN_FSTYPE_REASSOCREQ:
			fstype = "ReassocReq";
			break;
		case WLAN_FSTYPE_REASSOCRESP:
			fstype = "ReassocResp";
			break;
		case WLAN_FSTYPE_PROBEREQ:
			fstype = "ProbeReq";
			break;
		case WLAN_FSTYPE_PROBERESP:
			fstype = "ProbeResp";
			break;
		case WLAN_FSTYPE_BEACON:
			fstype = "Beacon";
			break;
		case WLAN_FSTYPE_ATIM:
			fstype = "ATIM";
			break;
		case WLAN_FSTYPE_DISASSOC:
			fstype = "Disassoc";
			break;
		case WLAN_FSTYPE_AUTHEN:
			fstype = "Authen";
			break;
		case WLAN_FSTYPE_DEAUTHEN:
			fstype = "Deauthen";
			break;
		}
		break;
	case WLAN_FTYPE_CTL:
		ftype = "CTL";
		switch (WLAN_GET_FC_FSTYPE(fc)) {
		case WLAN_FSTYPE_PSPOLL:
			fstype = "PSPoll";
			break;
		case WLAN_FSTYPE_RTS:
			fstype = "RTS";
			break;
		case WLAN_FSTYPE_CTS:
			fstype = "CTS";
			break;
		case WLAN_FSTYPE_ACK:
			fstype = "Ack";
			break;
		case WLAN_FSTYPE_CFEND:
			fstype = "CFEnd";
			break;
		case WLAN_FSTYPE_CFENDCFACK:
			fstype = "CFEndCFAck";
			break;
		}
		break;
	case WLAN_FTYPE_DATA:
		ftype = "DATA";
		switch (WLAN_GET_FC_FSTYPE(fc)) {
		case WLAN_FSTYPE_DATAONLY:
			fstype = "DataOnly";
			break;
		case WLAN_FSTYPE_DATA_CFACK:
			fstype = "Data CFAck";
			break;
		case WLAN_FSTYPE_DATA_CFPOLL:
			fstype = "Data CFPoll";
			break;
		case WLAN_FSTYPE_DATA_CFACK_CFPOLL:
			fstype = "Data CFAck/CFPoll";
			break;
		case WLAN_FSTYPE_NULL:
			fstype = "Null";
			break;
		case WLAN_FSTYPE_CFACK:
			fstype = "CFAck";
			break;
		case WLAN_FSTYPE_CFPOLL:
			fstype = "CFPoll";
			break;
		case WLAN_FSTYPE_CFACK_CFPOLL:
			fstype = "CFAck/CFPoll";
			break;
		}
		break;
	}
	sprintf(type_string, "%s/%s", ftype, fstype);
	FN_EXIT(1, (int)type_string);
	return type_string;
}
