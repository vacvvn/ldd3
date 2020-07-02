/*
 * snull.c --  the Simple Network Utility
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: snull.c,v 1.21 2004/11/05 02:36:03 rubini Exp $
 */
/*
Once you put these lines in /etc/networks, you can call your networks by name. 
snullnet0 192.168.0.0
snullnet1 192.168.1.0

The following are possible host numbers to put into /etc/hosts:
192.168.0.1 local0
192.168.0.2 remote0
192.168.1.2 local1
192.168.1.1 remote1

ifconfig sn0 local0
ifconfig sn1 local1

ping -c 2 remote0
ping -c 2 remote1
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */

#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>

#include "snull.h"

#include <linux/in6.h>
#include <asm/checksum.h>

MODULE_AUTHOR("Alessandro Rubini, Jonathan Corbet");
MODULE_LICENSE("Dual BSD/GPL");


/*
 * Transmitter lockup simulation, normally disabled.
 */
static int lockup = 0;
module_param(lockup, int, 0);

static int timeout = SNULL_TIMEOUT;
module_param(timeout, int, 0);

/*
 * Do we run in NAPI mode?
 */
static int use_napi = 0;
module_param(use_napi, int, 0);


/*
 * A structure representing an in-flight packet.
 */
struct snull_packet {
    struct snull_packet *next;
    struct net_device *dev;
    int	datalen;
    u8 data[ETH_DATA_LEN];
};

int pool_size = 8;
module_param(pool_size, int, 0);

/*
 * This structure is private to each device. It is used to pass
 * packets in and out, so there is place for a packet
 */

struct snull_priv {
    struct net_device_stats stats;
    int status;
    struct snull_packet *ppool;
    struct snull_packet *rx_queue;  /* List of incoming packets */
    int rx_int_enabled;
    int tx_packetlen;
    u8 *tx_packetdata;
    struct sk_buff *skb;
    spinlock_t lock;
    struct net_device *dev;
    struct napi_struct napi;
};

static void snull_tx_timeout(struct net_device *dev);
static void (*snull_interrupt)(int, void *, struct pt_regs *);

/*
 * Set up a device's packet pool.
 */
void snull_setup_pool(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    int i;
    struct snull_packet *pkt;
    printk(KERN_NOTICE"%s: snull_setup_pool. Pool_elements count: %d; Pool element size: %d",dev->name, pool_size, sizeof (struct snull_packet));
    priv->ppool = NULL;
    for (i = 0; i < pool_size; i++) {
        pkt = kmalloc (sizeof (struct snull_packet), GFP_KERNEL);
        if (pkt == NULL) {
            printk (KERN_NOTICE "Ran out of memory allocating packet pool\n");
            return;
        }
        pkt->dev = dev;
        pkt->next = priv->ppool;
        priv->ppool = pkt;
    }
}

void snull_teardown_pool(struct net_device *dev)
{
    printk(KERN_NOTICE"%s: snull_teardown_pool", dev->name);
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet *pkt;
    
    while ((pkt = priv->ppool)) {
        priv->ppool = pkt->next;
        kfree (pkt);
        /* FIXME - in-flight packets ? */
    }
}    

/*
 * Buffer/pool management.
 */
struct snull_packet *snull_get_tx_buffer(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    unsigned long flags;
    struct snull_packet *pkt;
    
    printk(KERN_NOTICE"%s, snull_get_tx_buffer", dev->name);
    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->ppool;
    priv->ppool = pkt->next;
    if (priv->ppool == NULL) {
        printk (KERN_INFO "Pool empty\n");
        netif_stop_queue(dev);
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}


void snull_release_buffer(struct snull_packet *pkt)
{
    unsigned long flags;

    printk(KERN_NOTICE"snull_release_buffer");
    struct snull_priv *priv = netdev_priv(pkt->dev);
    
    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->ppool;
    priv->ppool = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
    if (netif_queue_stopped(pkt->dev) && pkt->next == NULL)
        netif_wake_queue(pkt->dev);
}

void snull_enqueue_buf(struct net_device *dev, struct snull_packet *pkt)
{
    unsigned long flags;
    printk(KERN_NOTICE"%s: snull_enqueue_buf", dev->name);
    struct snull_priv *priv = netdev_priv(dev);

    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->rx_queue;  /* FIXME - misorders packets */
    priv->rx_queue = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
}

struct snull_packet *snull_dequeue_buf(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    printk(KERN_NOTICE"%s: snull_dequeue_buf", dev->name);
    struct snull_packet *pkt;
    unsigned long flags;

    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->rx_queue;
    if (pkt != NULL)
        priv->rx_queue = pkt->next;
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}

/*
 * Enable and disable receive interrupts.
 */
static void snull_rx_ints(struct net_device *dev, int enable)
{
    printk(KERN_NOTICE"%s: snull_rx_ints %s", dev->name,(enable == 1) ? "ena": "disa");
    struct snull_priv *priv = netdev_priv(dev);
    priv->rx_int_enabled = enable;
}

    
/*
 * Open and close
 */

int snull_open(struct net_device *dev)
{
    /* request_region(), request_irq(), ....  (like fops->open) */

    /* 
     * Assign the hardware address of the board: use "\0SNULx", where
     * x is 0 or 1. The first byte is '\0' to avoid being a multicast
     * address (the first byte of multicast addrs is odd).
     */
    printk(KERN_NOTICE"%s: snull open", dev->name);
    memcpy(dev->dev_addr, "\0SNUL0", ETH_ALEN);
    if (dev == snull_devs[1])
        dev->dev_addr[ETH_ALEN-1]++; /* \0SNUL1 */
    netif_start_queue(dev);
    return 0;
}

int snull_release(struct net_device *dev)
{
    /* release ports, irq and such -- like fops->close */

    printk(KERN_NOTICE"%s: snull release", dev->name);
    netif_stop_queue(dev); /* can't transmit any more */
    return 0;
}

/*
 * Configuration changes (passed on by ifconfig)
 */
int snull_config(struct net_device *dev, struct ifmap *map)
{
    printk(KERN_NOTICE"%s: snull config", dev->name);
    if (dev->flags & IFF_UP) /* can't act on a running interface */
        return -EBUSY;

    /* Don't allow changing the I/O address */
    if (map->base_addr != dev->base_addr) {
        printk(KERN_WARNING "snull: Can't change I/O address\n");
        return -EOPNOTSUPP;
    }

    /* Allow changing the IRQ */
    if (map->irq != dev->irq) {
        dev->irq = map->irq;
            /* request_irq() is delayed to open-time */
    }

    /* ignore other fields */
    return 0;
}

/*
 * Receive a packet: retrieve, encapsulate and pass over to upper levels
 */
void snull_rx(struct net_device *dev, struct snull_packet *pkt)
{
    struct sk_buff *skb;
    printk(KERN_NOTICE"%s: snull_rx", dev->name);
    struct snull_priv *priv = netdev_priv(dev);

    /*
     * The packet has been retrieved from the transmission
     * medium. Build an skb around it, so upper layers can handle it
     */
    skb = dev_alloc_skb(pkt->datalen + 2);
    if (!skb) {
        if (printk_ratelimit())
            printk(KERN_NOTICE "snull rx: low on mem - packet dropped\n");
        priv->stats.rx_dropped++;
        goto out;
    }
    skb_reserve(skb, 2); /* align IP on 16B boundary */  
    memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);

    /* Write metadata, and then pass to the receive level */
    skb->dev = dev;
    skb->protocol = eth_type_trans(skb, dev);
    skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
    priv->stats.rx_packets++;
    priv->stats.rx_bytes += pkt->datalen;
    netif_rx(skb);
  out:
    return;
}
    

/*
 * The poll implementation.
 */
static int snull_poll(struct napi_struct *napi, int budget)
{
    int npackets = 0;
    struct sk_buff *skb;
    struct snull_priv *priv = container_of(napi, struct snull_priv, napi);
    struct net_device *dev = priv->dev;
    printk(KERN_NOTICE"%s: snull_poll", dev->name);
    struct snull_packet *pkt;
    
    while (npackets < budget && priv->rx_queue) {
        pkt = snull_dequeue_buf(dev);
        skb = dev_alloc_skb(pkt->datalen + 2);
        if (! skb) {
            if (printk_ratelimit())
                printk(KERN_NOTICE "snull: packet dropped\n");
            priv->stats.rx_dropped++;
            snull_release_buffer(pkt);
            continue;
        }
        skb_reserve(skb, 2); /* align IP on 16B boundary */  
        memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);
        skb->dev = dev;
        skb->protocol = eth_type_trans(skb, dev);
        skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
        netif_receive_skb(skb);
        
            /* Maintain stats */
        npackets++;
        priv->stats.rx_packets++;
        priv->stats.rx_bytes += pkt->datalen;
        snull_release_buffer(pkt);
    }
    /* If we processed all packets, we're done; tell the kernel and reenable ints */
    if (! priv->rx_queue) {
        napi_complete(napi);
        snull_rx_ints(dev, 1);
        return 0;
    }
    /* We couldn't process everything. */
    return npackets;
}
        
        
/*
 * The typical interrupt entry point
 */
static void snull_regular_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    int statusword;
    struct snull_priv *priv;
    struct snull_packet *pkt = NULL;
    /*
     * As usual, check the "device" pointer to be sure it is
     * really interrupting.
     * Then assign "struct device *dev"
     */
    struct net_device *dev = (struct net_device *)dev_id;
    /* ... and check with hw if it's really ours */

    /* paranoid */
    if (!dev)
        return;

    /* Lock the device */
    priv = netdev_priv(dev);
    spin_lock(&priv->lock);

    /* retrieve statusword: real netdevices use I/O instructions */
    statusword = priv->status;
    priv->status = 0;
    printk(KERN_NOTICE"%s: snull_regular_interrupt ENTER; priv->status: 0x%x", dev->name, statusword);
    if (statusword & SNULL_RX_INTR) {
        /* send it to snull_rx for handling */
        pkt = priv->rx_queue;
        if (pkt) {
            priv->rx_queue = pkt->next;
            snull_rx(dev, pkt);
        }
    }
    if (statusword & SNULL_TX_INTR) {
        /* a transmission is over: free the skb */
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
        dev_kfree_skb(priv->skb);
    }

    /* Unlock the device and we are done */
    spin_unlock(&priv->lock);
    if (pkt) snull_release_buffer(pkt); /* Do this outside the lock! */

    printk(KERN_NOTICE"%s: snull_regular_interrupt EXIT; ", dev->name);
    return;
}

/*
 * A NAPI interrupt handler.
 */
static void snull_napi_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    int statusword;
    struct snull_priv *priv;

    /*
     * As usual, check the "device" pointer for shared handlers.
     * Then assign "struct device *dev"
     */
    struct net_device *dev = (struct net_device *)dev_id;
    printk(KERN_NOTICE"%s: snull_napi_interrupt", dev->name);
    /* ... and check with hw if it's really ours */

    /* paranoid */
    if (!dev)
        return;

    /* Lock the device */
    priv = netdev_priv(dev);
    spin_lock(&priv->lock);

    /* retrieve statusword: real netdevices use I/O instructions */
    statusword = priv->status;
    priv->status = 0;
    if (statusword & SNULL_RX_INTR) {
        snull_rx_ints(dev, 0);  /* Disable further interrupts */
        napi_schedule(&priv->napi);
    }
    if (statusword & SNULL_TX_INTR) {
            /* a transmission is over: free the skb */
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
        dev_kfree_skb(priv->skb);
    }

    /* Unlock the device and we are done */
    spin_unlock(&priv->lock);
    return;
}



/*
 * Transmit a packet (low level interface)
 */
static void snull_hw_tx(char *buf, int len, struct net_device *dev)
{
    /*
     * This function deals with hw details. This interface loops
     * back the packet to the other snull interface (if any).
     * In other words, this function implements the snull behaviour,
     * while all other procedures are rather device-independent
     */
    struct iphdr *ih;
    struct net_device *dest;
    struct snull_priv *priv;
    u32 *saddr, *daddr;
    struct snull_packet *tx_buffer;
    
    printk(KERN_NOTICE"%s: snull_hw_tx ENTER", dev->name);
    /* I am paranoid. Ain't I? */
    if (len < sizeof(struct ethhdr) + sizeof(struct iphdr)) {
        printk("snull: Hmm... packet too short (%i octets)\n",
                len);
        goto __LEAVE_HW_TX__;
    }

    if (1)
    { /* enable this conditional to look at the data */
        int i;
        // PDEBUG("len is %i\n" KERN_DEBUG "data:", len);
        u8 *dbg_buf = kmalloc((1 + (len * 2)), GFP_KERNEL);
        if (dbg_buf != NULL)
        {
            printk("\n");
            memset(dbg_buf, 0, ((len * 2) + 1));
            for (i = 0; i < len; i++)
                snprintf(&dbg_buf[i * 2], 3, "%02x", buf[i] & 0xff);
            printk(KERN_NOTICE "before: %s", dbg_buf);
            kfree(dbg_buf);
            dbg_buf = NULL;
        }
        else
        {
            printk(KERN_ALERT"snull_hw_tx cant alloc mem");
        }
        
    }
    /*
     * Ethhdr is 14 bytes, but the kernel arranges for iphdr
     * to be aligned (i.e., ethhdr is unaligned)
     */
    ih = (struct iphdr *)(buf+sizeof(struct ethhdr));
    saddr = &ih->saddr;
    daddr = &ih->daddr;

    ((u8 *)saddr)[2] ^= 1; /* change the third octet (class C) */
    ((u8 *)daddr)[2] ^= 1;

    ih->check = 0;         /* and rebuild the checksum (ip needs it) */
    ih->check = ip_fast_csum((unsigned char *)ih,ih->ihl);

    if (1)
    { /* enable this conditional to look at the data */
        int i;
        // PDEBUG("len is %i\n" KERN_DEBUG "data:", len);
        u8 *dbg_buf = kmalloc((1 + (len * 2)), GFP_KERNEL);
        if (dbg_buf != NULL)
        {
            memset(dbg_buf, 0, ((len * 2) + 1));
            for (i = 0; i < len; i++)
                snprintf(&dbg_buf[i * 2], 3, "%02x", buf[i] & 0xff);
            printk(KERN_NOTICE " after: %s", dbg_buf);
            kfree(dbg_buf);
            dbg_buf = NULL;
            printk("\n");
        }
        else
        {
            printk(KERN_ALERT"snull_hw_tx cant alloc mem");
        }
        
    }
    if (dev == snull_devs[0])
        PDEBUG("%08x:%05i --> %08x:%05i\n",
                ntohl(ih->saddr),ntohs(((struct tcphdr *)(ih+1))->source),
                ntohl(ih->daddr),ntohs(((struct tcphdr *)(ih+1))->dest));
    else
        PDEBUG("%08x:%05i <-- %08x:%05i\n",
                ntohl(ih->daddr),ntohs(((struct tcphdr *)(ih+1))->dest),
                ntohl(ih->saddr),ntohs(((struct tcphdr *)(ih+1))->source));

    /*
     * Ok, now the packet is ready for transmission: first simulate a
     * receive interrupt on the twin device, then  a
     * transmission-done on the transmitting device
     */
    dest = snull_devs[dev == snull_devs[0] ? 1 : 0];
    priv = netdev_priv(dest);
    tx_buffer = snull_get_tx_buffer(dev);
    tx_buffer->datalen = len;
    memcpy(tx_buffer->data, buf, len);
    snull_enqueue_buf(dest, tx_buffer);
    if (priv->rx_int_enabled) {
        priv->status |= SNULL_RX_INTR;
        snull_interrupt(0, dest, NULL);
    }

    priv = netdev_priv(dev);
    priv->tx_packetlen = len;
    priv->tx_packetdata = buf;
    priv->status |= SNULL_TX_INTR;
    if (lockup && ((priv->stats.tx_packets + 1) % lockup) == 0) {
            /* Simulate a dropped transmit interrupt */
        netif_stop_queue(dev);
        PDEBUG("Simulate lockup at %ld, txp %ld\n", jiffies,
                (unsigned long) priv->stats.tx_packets);
    }
    else
        snull_interrupt(0, dev, NULL);
__LEAVE_HW_TX__:
    printk(KERN_NOTICE"%s: snull_hw_tx EXIT", dev->name);
}

/*
 * Transmit a packet (called by the kernel)
 */
int snull_tx(struct sk_buff *skb, struct net_device *dev)
{
    printk(KERN_NOTICE"%s: snull_tx", dev->name);
    int len;
    char *data, shortpkt[ETH_ZLEN];
    struct snull_priv *priv = netdev_priv(dev);

    data = skb->data;
    len = skb->len;
    if (len < ETH_ZLEN) {
        memset(shortpkt, 0, ETH_ZLEN);
        memcpy(shortpkt, skb->data, skb->len);
        len = ETH_ZLEN;
        data = shortpkt;
    }
    netif_trans_update(dev);

    /* Remember the skb, so we can free it at interrupt time */
    priv->skb = skb;

    /* actual deliver of data is device-specific, and not shown here */
    snull_hw_tx(data, len, dev);

    return 0; /* Our simple device can not fail */
}

/*
 * Deal with a transmit timeout.
 */
void snull_tx_timeout (struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);

    printk(KERN_NOTICE"%s: snull_tx_timeout", dev->name);
    // PDEBUG(KERN_NOTICE"Transmit timeout at %ld, latency %ld\n", jiffies,
    //         jiffies - dev->trans_start);
        /* Simulate a transmission interrupt to get things moving */
    priv->status = SNULL_TX_INTR;
    snull_interrupt(0, dev, NULL);
    priv->stats.tx_errors++;
    netif_wake_queue(dev);
    return;
}



/*
 * Ioctl commands 
 */
int snull_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    PDEBUG("ioctl\n");
    // printk(KERN_NOTICE"%s: ioctl: 0x%x", dev->name, cmd);	
    return 0;
}

/*
 * Return statistics to the caller
 */
struct net_device_stats *snull_stats(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    if(priv->status != 0)
        printk(KERN_NOTICE"%s: snull_stats: 0x%08x", dev->name, priv->status);
    return &priv->stats;
}

/*
 * This function is called to fill up an eth header, since arp is not
 * available on the interface
 */
int snull_rebuild_header(struct sk_buff *skb)
{
    struct ethhdr *eth = (struct ethhdr *) skb->data;
    struct net_device *dev = skb->dev;
    
    printk(KERN_NOTICE"%s: snull_rebuild_header", dev->name);
    memcpy(eth->h_source, dev->dev_addr, dev->addr_len);
    memcpy(eth->h_dest, dev->dev_addr, dev->addr_len);
    eth->h_dest[ETH_ALEN-1]   ^= 0x01;   /* dest is us xor 1 */
    return 0;
}


int snull_header(struct sk_buff *skb, struct net_device *dev,
                unsigned short type, const void *daddr, const void *saddr,
                unsigned len)
{
    struct ethhdr *eth = (struct ethhdr *)skb_push(skb,ETH_HLEN);

    printk(KERN_NOTICE"%s: snull_header ENTER", dev->name);
    printk(KERN_NOTICE"dev mac addr:\t%02x:%02x:%02x:%02x:%02x:%02x",dev->dev_addr[0],dev->dev_addr[1],dev->dev_addr[2],dev->dev_addr[3],dev->dev_addr[4],dev->dev_addr[5]);
    eth->h_proto = htons(type);
	//copy mac-addresses
    memcpy(eth->h_source, saddr ? saddr : dev->dev_addr, dev->addr_len);
    if(saddr == NULL)
        printk(KERN_NOTICE"saddr param is NULL, copy dev_addr to source addr header field");
    printk(KERN_NOTICE"source mac addr:\t%02x:%02x:%02x:%02x:%02x:%02x",eth->h_source[0],eth->h_source[1],eth->h_source[2],eth->h_source[3],eth->h_source[4],eth->h_source[5]);
    memcpy(eth->h_dest,   daddr ? daddr : dev->dev_addr, dev->addr_len);
    if(daddr == NULL)
        printk(KERN_NOTICE"daddr param is NULL, copy dev_addr to dest addr header field");
    printk(KERN_NOTICE"dest mac addr before:\t%02x:%02x:%02x:%02x:%02x:%02x",eth->h_dest[0],eth->h_dest[1],eth->h_dest[2],eth->h_dest[3],eth->h_dest[4],eth->h_dest[5]);
    eth->h_dest[ETH_ALEN-1]   ^= 0x01;   /* dest is us xor 1 */
    printk(KERN_NOTICE"dest mac addr after:\t%02x:%02x:%02x:%02x:%02x:%02x",eth->h_dest[0],eth->h_dest[1],eth->h_dest[2],eth->h_dest[3],eth->h_dest[4],eth->h_dest[5]);
    printk(KERN_NOTICE"%s: snull_header EXIT", dev->name);
    return (dev->hard_header_len);
}





/*
 * The "change_mtu" method is usually not needed.
 * If you need it, it must be like this.
 */
int snull_change_mtu(struct net_device *dev, int new_mtu)
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(dev);
    spinlock_t *lock = &priv->lock;
    
    printk(KERN_NOTICE"%s: snull_change_mtu", dev->name);
    /* check ranges */
    if ((new_mtu < 68) || (new_mtu > 1500))
        return -EINVAL;
    /*
     * Do anything you need, and the accept the value
     */
    spin_lock_irqsave(lock, flags);
    dev->mtu = new_mtu;
    spin_unlock_irqrestore(lock, flags);
    return 0; /* success */
}

static const struct header_ops snull_header_ops = {
        .create  = snull_header,
};

static const struct net_device_ops snull_netdev_ops = {
    .ndo_open            = snull_open,
    .ndo_stop            = snull_release,
    .ndo_start_xmit      = snull_tx,
    .ndo_do_ioctl        = snull_ioctl,
    .ndo_set_config      = snull_config,
    .ndo_get_stats       = snull_stats,
    .ndo_change_mtu      = snull_change_mtu,
    .ndo_tx_timeout      = snull_tx_timeout
};

/*
 * The init function (sometimes called probe).
 * It is invoked by register_netdev()
 */
void snull_init(struct net_device *dev)
{
    struct snull_priv *priv;
    printk(KERN_NOTICE"snull init");
#if 0
        /*
     * Make the usual checks: check_region(), probe irq, ...  -ENODEV
     * should be returned if no device found.  No resource should be
     * grabbed: this is done on open(). 
     */
#endif

        /* 
     * Then, assign other fields in dev, using ether_setup() and some
     * hand assignments
     */
    ether_setup(dev); /* assign some of the fields */
    dev->watchdog_timeo = timeout;
    dev->netdev_ops = &snull_netdev_ops;
    dev->header_ops = &snull_header_ops;
    /* keep the default flags, just add NOARP */
    dev->flags           |= IFF_NOARP;
    dev->features        |= NETIF_F_HW_CSUM;

    /*
     * Then, initialize the priv field. This encloses the statistics
     * and a few private fields.
     */
    priv = netdev_priv(dev);
    if (use_napi) {
        netif_napi_add(dev, &priv->napi, snull_poll,2);
    }
    memset(priv, 0, sizeof(struct snull_priv));
    spin_lock_init(&priv->lock);
    snull_rx_ints(dev, 1);		/* enable receive interrupts */
    snull_setup_pool(dev);
}

/*
 * The devices
 */

struct net_device *snull_devs[2];



/*
 * Finally, the module stuff
 */

void snull_cleanup(void)
{
    int i;
    printk(KERN_NOTICE"snull_cleanup");
    for (i = 0; i < 2;  i++) {
        if (snull_devs[i]) {
            unregister_netdev(snull_devs[i]);
            snull_teardown_pool(snull_devs[i]);
            free_netdev(snull_devs[i]);
        }
    }
    return;
}




int snull_init_module(void)
{
    int result, i, ret = -ENOMEM;

    printk(KERN_NOTICE"snull init module");
    snull_interrupt = use_napi ? snull_napi_interrupt : snull_regular_interrupt;

    /* Allocate the devices */
    snull_devs[0] = alloc_netdev(sizeof(struct snull_priv), "sn%d",
            NET_NAME_UNKNOWN, snull_init);
    snull_devs[1] = alloc_netdev(sizeof(struct snull_priv), "sn%d",
            NET_NAME_UNKNOWN, snull_init);
    if (snull_devs[0] == NULL || snull_devs[1] == NULL)
        goto out;

    ret = -ENODEV;
    for (i = 0; i < 2;  i++)
        if ((result = register_netdev(snull_devs[i])))
            printk("snull: error %i registering device \"%s\"\n",
                    result, snull_devs[i]->name);
        else
            ret = 0;
   out:
    if (ret) 
        snull_cleanup();
    return ret;
}


module_init(snull_init_module);
module_exit(snull_cleanup);
