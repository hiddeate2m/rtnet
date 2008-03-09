/***
 *
 *  rtcap/rtcap.c
 *
 *  Real-Time Capturing Interface
 *
 *  Copyright (C) 2004, 2005 Jan Kiszka <jan.kiszka@web.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/sched.h>

#include <rtdev.h>
#include <rtnet_chrdev.h>


#ifdef CONFIG_RTOS_STARTSTOP_TIMER
static int start_timer = 0;

module_param(start_timer, int, 0444);
MODULE_PARM_DESC(start_timer, "set to non-zero if RTAI timer was not yet "
                 "started");
#endif

MODULE_LICENSE("GPL");

static unsigned int rtcap_rtskbs = 128;
module_param(rtcap_rtskbs, uint, 0444);
MODULE_PARM_DESC(rtcap_rtskbs, "Number of real-time socket buffers per "
                 "real-time device");

#define TAP_DEV             1
#define RTMAC_TAP_DEV       2
#define XMIT_HOOK           4

static rtdm_nrtsig_t        cap_signal;
static struct rtskb_queue   cap_queue;
static struct rtskb_queue   cap_pool;

static struct tap_device_t {
    struct net_device       *tap_dev;
    struct net_device       *rtmac_tap_dev;
    struct net_device_stats tap_dev_stats;
    int                     present;
    int                     (*orig_xmit)(struct rtskb *skb,
                                         struct rtnet_device *dev);
} tap_device[MAX_RT_DEVICES];



void rtcap_rx_hook(struct rtskb *rtskb)
{
    if ((rtskb->cap_comp_skb = rtskb_dequeue(&cap_pool)) == 0) {
        tap_device[rtskb->rtdev->ifindex].tap_dev_stats.rx_dropped++;
        return;
    }
#ifdef CONFIG_RTNET_CHECKED
    cap_pool.pool_balance--;
#endif

    if (cap_queue.first == NULL)
        cap_queue.first = rtskb;
    else
        cap_queue.last->cap_next = rtskb;
    cap_queue.last  = rtskb;
    rtskb->cap_next = NULL;

    rtskb->cap_flags |= RTSKB_CAP_SHARED;

    rtdm_nrtsig_pend(&cap_signal);
}



int rtcap_xmit_hook(struct rtskb *rtskb, struct rtnet_device *rtdev)
{
    struct tap_device_t *tap_dev = &tap_device[rtskb->rtdev->ifindex];
    rtdm_lockctx_t      context;


    if ((rtskb->cap_comp_skb = rtskb_dequeue(&cap_pool)) == 0) {
        tap_dev->tap_dev_stats.rx_dropped++;
        return tap_dev->orig_xmit(rtskb, rtdev);
    }
#ifdef CONFIG_RTNET_CHECKED
    cap_pool.pool_balance--;
#endif

    rtskb->cap_next  = NULL;
    rtskb->cap_start = rtskb->data;
    rtskb->cap_len   = rtskb->len;
    rtskb->cap_flags |= RTSKB_CAP_SHARED;

    rtskb->time_stamp = rtdm_clock_read();

    rtdm_lock_get_irqsave(&rtcap_lock, context);

    if (cap_queue.first == NULL)
        cap_queue.first = rtskb;
    else
        cap_queue.last->cap_next = rtskb;
    cap_queue.last = rtskb;

    rtdm_lock_put_irqrestore(&rtcap_lock, context);

    rtdm_nrtsig_pend(&cap_signal);

    return tap_dev->orig_xmit(rtskb, rtdev);
}



int rtcap_loopback_xmit_hook(struct rtskb *rtskb, struct rtnet_device *rtdev)
{
    struct tap_device_t *tap_dev = &tap_device[rtskb->rtdev->ifindex];


    rtskb->time_stamp = rtdm_clock_read();

    return tap_dev->orig_xmit(rtskb, rtdev);
}



void rtcap_kfree_rtskb(struct rtskb *rtskb)
{
    rtdm_lockctx_t  context;
    struct rtskb    *comp_skb;


    rtdm_lock_get_irqsave(&rtcap_lock, context);

    if (rtskb->cap_flags & RTSKB_CAP_SHARED) {
        rtskb->cap_flags &= ~RTSKB_CAP_SHARED;

        comp_skb = rtskb->cap_comp_skb;

        rtdm_lock_put_irqrestore(&rtcap_lock, context);

        rtskb_queue_tail(comp_skb->pool, comp_skb);
#ifdef CONFIG_RTNET_CHECKED
        comp_skb->pool->pool_balance++;
#endif

        return;
    }

    rtdm_lock_put_irqrestore(&rtcap_lock, context);

    rtskb->chain_end = rtskb;
    rtskb_queue_tail(rtskb->pool, rtskb);
#ifdef CONFIG_RTNET_CHECKED
    rtskb->pool->pool_balance++;
#endif
}



static void convert_timestamp(nanosecs_abs_t timestamp, struct sk_buff *skb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
# ifdef CONFIG_KTIME_SCALAR
    skb->tstamp.tv64 = timestamp;
# else /* !CONFIG_KTIME_SCALAR */
    unsigned long rem;

    rem = do_div(timestamp, NSEC_PER_SEC);
    skb->tstamp = ktime_set((long)timestamp, rem);
# endif /* !CONFIG_KTIME_SCALAR */
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,14)
    struct timeval tv;

    tv.tv_usec = do_div(timestamp, NSEC_PER_SEC);
    tv.tv_sec = (long)timestamp;
    tv.tv_usec /= 1000;
    skb_set_timestamp(skb, &tv);
#else /* KERNEL_VERSION < 2.6.14 */
# ifndef NSEC_PER_SEC
#  define NSEC_PER_SEC (1000000000L)
# endif
    skb->stamp.tv_usec = do_div(timestamp, NSEC_PER_SEC);
    skb->stamp.tv_sec = (long)timestamp;
    skb->stamp.tv_usec /= 1000;
#endif
}



static void rtcap_signal_handler(rtdm_nrtsig_t nrtsig, void *arg)
{
    struct rtskb            *rtskb;
    struct sk_buff          *skb;
    struct sk_buff          *rtmac_skb;
    struct net_device_stats *stats;
    int                     ifindex;
    int                     active;
    rtdm_lockctx_t          context;


    while (1)
    {
        rtdm_lock_get_irqsave(&rtcap_lock, context);

        if ((rtskb = cap_queue.first) == NULL) {
            rtdm_lock_put_irqrestore(&rtcap_lock, context);
            break;
        }

        cap_queue.first = rtskb->cap_next;

        rtdm_lock_put_irqrestore(&rtcap_lock, context);

        ifindex = rtskb->rtdev->ifindex;
        active  = tap_device[ifindex].present;

        if ((tap_device[ifindex].tap_dev->flags & IFF_UP) == 0)
            active &= ~TAP_DEV;
        if (active & RTMAC_TAP_DEV &&
            !(tap_device[ifindex].rtmac_tap_dev->flags & IFF_UP))
            active &= ~RTMAC_TAP_DEV;

        if (active == 0) {
            tap_device[ifindex].tap_dev_stats.rx_dropped++;
            rtcap_kfree_rtskb(rtskb);
            continue;
        }

        skb = dev_alloc_skb(rtskb->cap_len);
        if (skb) {
            memcpy(skb_put(skb, rtskb->cap_len),
                   rtskb->cap_start, rtskb->cap_len);

            if (active & TAP_DEV) {
                skb->dev      = tap_device[ifindex].tap_dev;
                skb->protocol = eth_type_trans(skb, skb->dev);
                convert_timestamp(rtskb->time_stamp, skb);

                rtmac_skb = NULL;
                if ((rtskb->cap_flags & RTSKB_CAP_RTMAC_STAMP) &&
                    (active & RTMAC_TAP_DEV)) {
                    rtmac_skb = skb_clone(skb, GFP_ATOMIC);
                    if (rtmac_skb != NULL)
                        convert_timestamp(rtskb->cap_rtmac_stamp, rtmac_skb);
                }

                rtcap_kfree_rtskb(rtskb);

                stats = &tap_device[ifindex].tap_dev_stats;
                stats->rx_packets++;
                stats->rx_bytes += skb->len;

                if (rtmac_skb != NULL) {
                    rtmac_skb->dev = tap_device[ifindex].rtmac_tap_dev;
                    netif_rx(rtmac_skb);
                }
                netif_rx(skb);
            } else if (rtskb->cap_flags & RTSKB_CAP_RTMAC_STAMP) {
                skb->dev      = tap_device[ifindex].rtmac_tap_dev;
                skb->protocol = eth_type_trans(skb, skb->dev);
                convert_timestamp(rtskb->cap_rtmac_stamp, skb);

                rtcap_kfree_rtskb(rtskb);

                stats = &tap_device[ifindex].tap_dev_stats;
                stats->rx_packets++;
                stats->rx_bytes += skb->len;

                netif_rx(skb);
            } else {
                dev_kfree_skb(skb);
                rtcap_kfree_rtskb(rtskb);
            }
        } else {
            printk("RTcap: unable to allocate linux skb\n");
            rtcap_kfree_rtskb(rtskb);
        }
    }
}



static int tap_dev_open(struct net_device *dev)
{
    memcpy(dev->dev_addr, ((struct rtnet_device*)dev->priv)->dev_addr,
           sizeof(dev->dev_addr));

    return 0;
}



static int tap_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
    netif_stop_queue(dev);
    return 1;
}



static struct net_device_stats *tap_dev_get_stats(struct net_device *dev)
{
    struct rtnet_device *rtdev = (struct rtnet_device*)dev->priv;

    return &tap_device[rtdev->ifindex].tap_dev_stats;
}



static int tap_dev_change_mtu(struct net_device *dev, int new_mtu)
{
    return -EINVAL;
}



static void tap_dev_setup(struct net_device *dev)
{
    ether_setup(dev);

    dev->open            = tap_dev_open;
    dev->hard_start_xmit = tap_dev_xmit;
    dev->get_stats       = tap_dev_get_stats;
    dev->change_mtu      = tap_dev_change_mtu;
    dev->set_mac_address = NULL;
    dev->mtu             = 1500;
    dev->flags           &= ~IFF_MULTICAST;

#ifdef HAVE_VALIDATE_ADDR
    dev->validate_addr = NULL;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
    SET_MODULE_OWNER(dev);
#endif
}



void cleanup_tap_devices(void)
{
    int                 i;
    struct rtnet_device *rtdev;


    for (i = 0; i < MAX_RT_DEVICES; i++)
        if ((tap_device[i].present & TAP_DEV) != 0) {
            if ((tap_device[i].present & XMIT_HOOK) != 0) {
                rtdev = (struct rtnet_device *)tap_device[i].tap_dev->priv;

                down(&rtdev->nrt_lock);
                rtdev->hard_start_xmit = tap_device[i].orig_xmit;
                if (rtdev->features & NETIF_F_LLTX)
                    rtdev->start_xmit = tap_device[i].orig_xmit;
                RTNET_MOD_DEC_USE_COUNT_EX(rtdev->rt_owner);
                up(&rtdev->nrt_lock);

                rtdev_dereference(rtdev);
            }

            if ((tap_device[i].present & RTMAC_TAP_DEV) != 0) {
                unregister_netdev(tap_device[i].rtmac_tap_dev);
                free_netdev(tap_device[i].rtmac_tap_dev);
            }

            unregister_netdev(tap_device[i].tap_dev);
            free_netdev(tap_device[i].tap_dev);
        }
}



int __init rtcap_init(void)
{
    struct rtnet_device *rtdev;
    struct net_device   *dev;
    int                 ret;
    int                 devices = 0;
    int                 i;


    printk("RTcap: real-time capturing interface\n");

#ifdef CONFIG_RTOS_STARTSTOP_TIMER
    if (start_timer)
        rtos_timer_start();
#endif

    rtskb_queue_init(&cap_queue);

    ret = rtdm_nrtsig_init(&cap_signal, rtcap_signal_handler, NULL);
    if (ret < 0)
        goto error1;

    for (i = 0; i < MAX_RT_DEVICES; i++) {
        tap_device[i].present = 0;

        rtdev = rtdev_get_by_index(i);
        if (rtdev != NULL) {
            down(&rtdev->nrt_lock);

            if (test_bit(PRIV_FLAG_UP, &rtdev->priv_flags)) {
                up(&rtdev->nrt_lock);
                printk("RTcap: %s busy, skipping device!\n", rtdev->name);
                rtdev_dereference(rtdev);
                continue;
            }

            if (rtdev->mac_priv != NULL) {
                up(&rtdev->nrt_lock);

                printk("RTcap: RTmac discipline already active on device %s. "
                       "Load RTcap before RTmac!\n", rtdev->name);

                rtdev_dereference(rtdev);
                continue;
            }

            memset(&tap_device[i].tap_dev_stats, 0,
                   sizeof(struct net_device_stats));

            dev = alloc_netdev(0, rtdev->name, tap_dev_setup);
            if (!dev) {
                ret = -ENOMEM;
                goto error3;
            }

            tap_device[i].tap_dev = dev;
            dev->priv = rtdev;

            ret = register_netdev(dev);
            if (ret < 0)
                goto error3;

            tap_device[i].present = TAP_DEV;

            tap_device[i].orig_xmit = rtdev->hard_start_xmit;

            if ((rtdev->flags & IFF_LOOPBACK) == 0) {
                dev = alloc_netdev(0, rtdev->name, tap_dev_setup);
                if (!dev) {
                    ret = -ENOMEM;
                    goto error3;
                }

                tap_device[i].rtmac_tap_dev = dev;
                dev->priv = rtdev;
                strncat(dev->name, "-mac", IFNAMSIZ-strlen(dev->name));

                ret = register_netdev(dev);
                if (ret < 0)
                    goto error3;

                tap_device[i].present |= RTMAC_TAP_DEV;

                rtdev->hard_start_xmit = rtcap_xmit_hook;
            } else
                rtdev->hard_start_xmit = rtcap_loopback_xmit_hook;

            /* If the device requires no xmit_lock, start_xmit points equals
             * hard_start_xmit => we have to update this as well
             */
            if (rtdev->features & NETIF_F_LLTX)
                rtdev->start_xmit = rtdev->hard_start_xmit;

            tap_device[i].present |= XMIT_HOOK;
            RTNET_MOD_INC_USE_COUNT_EX(rtdev->rt_owner);

            up(&rtdev->nrt_lock);

            devices++;
        }
    }

    if (devices == 0) {
        printk("RTcap: no real-time devices found!\n");
        ret = -ENODEV;
        goto error2;
    }

    if (rtskb_pool_init(&cap_pool, rtcap_rtskbs * devices) <
            rtcap_rtskbs * devices) {
        rtskb_pool_release(&cap_pool);
        ret = -ENOMEM;
        goto error2;
    }

    /* register capturing handlers with RTnet core
     * (adding the handler need no locking) */
    rtcap_handler = rtcap_rx_hook;

    return 0;

  error3:
    up(&rtdev->nrt_lock);
    rtdev_dereference(rtdev);
    printk("RTcap: unable to register %s!\n", dev->name);

  error2:
    cleanup_tap_devices();
    rtdm_nrtsig_destroy(&cap_signal);

  error1:
#ifdef CONFIG_RTOS_STARTSTOP_TIMER
    if (start_timer)
        rtos_timer_stop();
#endif

    return ret;
}



void rtcap_cleanup(void)
{
    rtdm_lockctx_t  context;


#ifdef CONFIG_RTOS_STARTSTOP_TIMER
    if (start_timer)
        rtos_timer_stop();
#endif

    rtdm_nrtsig_destroy(&cap_signal);

    /* unregister capturing handlers
     * (take lock to avoid any unloading code before handler was left) */
    rtdm_lock_get_irqsave(&rtcap_lock, context);
    rtcap_handler = NULL;
    rtdm_lock_put_irqrestore(&rtcap_lock, context);

    /* empty queue (should be already empty) */
    rtcap_signal_handler(0, NULL /* we ignore them anyway */);

    cleanup_tap_devices();

    rtskb_pool_release(&cap_pool);

    printk("RTcap: unloaded\n");
}



module_init(rtcap_init);
module_exit(rtcap_cleanup);
