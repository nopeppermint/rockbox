/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Bj�rn Stenberg
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "thread.h"
#include "cpu.h"
#include "system.h"
#include "panic.h"
#if CONFIG_CPU == IMX31L
#include "avic-imx31.h"
#endif

#if (!defined(CPU_PP) && (CONFIG_CPU != IMX31L)) || !defined(BOOTLOADER) 
volatile long current_tick NOCACHEDATA_ATTR = 0;
#endif

void (*tick_funcs[MAX_NUM_TICK_TASKS])(void);

/* This array holds all queues that are initiated. It is used for broadcast. */
static struct event_queue *all_queues[32] NOCACHEBSS_ATTR;
static int num_queues NOCACHEBSS_ATTR;

/****************************************************************************
 * Standard kernel stuff
 ****************************************************************************/
void kernel_init(void)
{
    /* Init the threading API */
#if NUM_CORES > 1
    if (CURRENT_CORE == COP)
    {
        /* This enables the interrupt but it won't be active until
           the timer is actually started and interrupts are unmasked */
        tick_start(1000/HZ);
    }
#endif

    init_threads();

    /* No processor other than the CPU will proceed here */
    memset(tick_funcs, 0, sizeof(tick_funcs));
    num_queues = 0;
    memset(all_queues, 0, sizeof(all_queues));
    tick_start(1000/HZ);
}

void sleep(int ticks)
{
#if CONFIG_CPU == S3C2440 && defined(BOOTLOADER)
    volatile int counter;
    TCON &= ~(1 << 20); // stop timer 4
    // TODO: this constant depends on dividers settings inherited from
    // firmware. Set them explicitly somwhere.
    TCNTB4 = 12193 * ticks / HZ;
    TCON |= 1 << 21; // set manual bit
    TCON &= ~(1 << 21); // reset manual bit
    TCON &= ~(1 << 22); //autoreload Off
    TCON |= (1 << 20); // start timer 4
    do {
       counter = TCNTO4;
    } while(counter > 0);

#elif defined(CPU_PP) && defined(BOOTLOADER)
    unsigned stop = USEC_TIMER + ticks * (1000000/HZ);
    while (TIME_BEFORE(USEC_TIMER, stop))
        switch_thread(true,NULL);
#else
    sleep_thread(ticks);
#endif
}

void yield(void)
{
#if ((CONFIG_CPU == S3C2440 || defined(ELIO_TPJ1022) || CONFIG_CPU == IMX31L) && defined(BOOTLOADER))
    /* Some targets don't like yielding in the bootloader */
#else
    switch_thread(true, NULL);
#endif
}

/****************************************************************************
 * Queue handling stuff
 ****************************************************************************/

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
/* Moves waiting thread's descriptor to the current sender when a
   message is dequeued */
static void queue_fetch_sender(struct queue_sender_list *send,
                               unsigned int i)
{
    struct thread_entry **spp = &send->senders[i];

    if (*spp)
    {
        send->curr_sender = *spp;
        *spp = NULL;
    }
}

/* Puts the specified return value in the waiting thread's return value
 * and wakes the thread.
 * 1) A sender should be confirmed to exist before calling which makes it
 *    more efficent to reject the majority of cases that don't need this
      called.
 * 2) Requires interrupts disabled since queue overflows can cause posts
 *    from interrupt handlers to wake threads. Not doing so could cause
 *    an attempt at multiple wakes or other problems.
 */
static void queue_release_sender(struct thread_entry **sender,
                                 intptr_t retval)
{
    (*sender)->retval = retval;
    wakeup_thread_irq_safe(sender);
#if 0
    /* This should _never_ happen - there must never be multiple
       threads in this list and it is a corrupt state */
    if (*sender != NULL)
        panicf("Queue: send slot ovf");
#endif
}

/* Releases any waiting threads that are queued with queue_send -
 * reply with 0.
 * Disable IRQs before calling since it uses queue_release_sender.
 */
static void queue_release_all_senders(struct event_queue *q)
{
    if(q->send)
    {
        unsigned int i;
        for(i = q->read; i != q->write; i++)
        {
            struct thread_entry **spp =
                &q->send->senders[i & QUEUE_LENGTH_MASK];

            if(*spp)
            {
                queue_release_sender(spp, 0);
            }
        }
    }
}

/* Enables queue_send on the specified queue - caller allocates the extra
   data structure */
void queue_enable_queue_send(struct event_queue *q,
                             struct queue_sender_list *send)
{
    q->send = send;
    memset(send, 0, sizeof(struct queue_sender_list));
}
#endif /* HAVE_EXTENDED_MESSAGING_AND_NAME */


void queue_init(struct event_queue *q, bool register_queue)
{
    q->read   = 0;
    q->write  = 0;
    q->thread = NULL;
#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    q->send   = NULL; /* No message sending by default */
#endif

    if(register_queue)
    {
        /* Add it to the all_queues array */
        all_queues[num_queues++] = q;
    }
}

void queue_delete(struct event_queue *q)
{
    int i;
    bool found = false;

    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    /* Release theads waiting on queue */
    wakeup_thread(&q->thread);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    /* Release waiting threads and reply to any dequeued message
       waiting for one. */
    queue_release_all_senders(q);
    queue_reply(q, 0);
#endif
    
    /* Find the queue to be deleted */
    for(i = 0;i < num_queues;i++)
    {
        if(all_queues[i] == q)
        {
            found = true;
            break;
        }
    }

    if(found)
    {
        /* Move the following queues up in the list */
        for(;i < num_queues-1;i++)
        {
            all_queues[i] = all_queues[i+1];
        }
        
        num_queues--;
    }
    
    set_irq_level(oldlevel);
}

void queue_wait(struct event_queue *q, struct event *ev)
{
    int oldlevel;
    unsigned int rd;

    oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if(q->send && q->send->curr_sender)
    {
        /* auto-reply */
        queue_release_sender(&q->send->curr_sender, 0);
    }
#endif
    
    if (q->read == q->write)
    {
        set_irq_level_and_block_thread(&q->thread, oldlevel);
        oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    } 

    rd = q->read++ & QUEUE_LENGTH_MASK;
    *ev = q->events[rd];

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if(q->send && q->send->senders[rd])
    {
        /* Get data for a waiting thread if one */
        queue_fetch_sender(q->send, rd);
    }
#endif
    
    set_irq_level(oldlevel);
}

void queue_wait_w_tmo(struct event_queue *q, struct event *ev, int ticks)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if (q->send && q->send->curr_sender)
    {
        /* auto-reply */
        queue_release_sender(&q->send->curr_sender, 0);
    }
#endif
    
    if (q->read == q->write && ticks > 0)
    {
        set_irq_level_and_block_thread_w_tmo(&q->thread, ticks, oldlevel);
        oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    }

    if (q->read != q->write)
    {
        unsigned int rd = q->read++ & QUEUE_LENGTH_MASK;
        *ev = q->events[rd];

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
        if(q->send && q->send->senders[rd])
        {
            /* Get data for a waiting thread if one */
            queue_fetch_sender(q->send, rd);
        }
#endif
    }
    else
    {
        ev->id = SYS_TIMEOUT;
    }
    
    set_irq_level(oldlevel);
}

void queue_post(struct event_queue *q, long id, intptr_t data)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    unsigned int wr;
    
    wr = q->write++ & QUEUE_LENGTH_MASK;

    q->events[wr].id   = id;
    q->events[wr].data = data;

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    if(q->send)
    {
        struct thread_entry **spp = &q->send->senders[wr];

        if (*spp)
        {
            /* overflow protect - unblock any thread waiting at this index */
            queue_release_sender(spp, 0);
        }
    }
#endif

    wakeup_thread_irq_safe(&q->thread);
    set_irq_level(oldlevel);
    
}

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
/* No wakeup_thread_irq_safe here because IRQ handlers are not allowed
   use of this function - we only aim to protect the queue integrity by
   turning them off. */
intptr_t queue_send(struct event_queue *q, long id, intptr_t data)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);
    unsigned int wr;

    wr = q->write++ & QUEUE_LENGTH_MASK;

    q->events[wr].id   = id;
    q->events[wr].data = data;
    
    if(q->send)
    {
        struct thread_entry **spp = &q->send->senders[wr];

        if (*spp)
        {
            /* overflow protect - unblock any thread waiting at this index */
            queue_release_sender(spp, 0);
        }

        wakeup_thread(&q->thread);
        set_irq_level_and_block_thread(spp, oldlevel);
        return thread_get_current()->retval;
    }

    /* Function as queue_post if sending is not enabled */
    wakeup_thread(&q->thread);
    set_irq_level(oldlevel);
    
    return 0;
}

#if 0 /* not used now but probably will be later */
/* Query if the last message dequeued was added by queue_send or not */
bool queue_in_queue_send(struct event_queue *q)
{
    return q->send && q->send->curr_sender;
}
#endif

/* Replies with retval to any dequeued message sent with queue_send */
void queue_reply(struct event_queue *q, intptr_t retval)
{
    /* No IRQ lock here since IRQs cannot change this */
    if(q->send && q->send->curr_sender)
    {
        queue_release_sender(&q->send->curr_sender, retval);
    }
}
#endif /* HAVE_EXTENDED_MESSAGING_AND_NAME */

bool queue_empty(const struct event_queue* q)
{
    return ( q->read == q->write );
}

void queue_clear(struct event_queue* q)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
    /* Release all thread waiting in the queue for a reply -
       dequeued sent message will be handled by owning thread */
    queue_release_all_senders(q);
#endif

    q->read = 0;
    q->write = 0;
    
    set_irq_level(oldlevel);
}

void queue_remove_from_head(struct event_queue *q, long id)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    while(q->read != q->write)
    {
        unsigned int rd = q->read & QUEUE_LENGTH_MASK;

        if(q->events[rd].id != id)
        {
            break;
        }

#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
        if(q->send)
        {
            struct thread_entry **spp = &q->send->senders[rd];

            if (*spp)
            {
                /* Release any thread waiting on this message */
                queue_release_sender(spp, 0);
            }
        }
#endif
        q->read++;
    }
    
    set_irq_level(oldlevel);
}

/**
 * The number of events waiting in the queue.
 * 
 * @param struct of event_queue
 * @return number of events in the queue
 */
int queue_count(const struct event_queue *q)
{
    return q->write - q->read;
}

int queue_broadcast(long id, intptr_t data)
{
    int i;
    
    for(i = 0;i < num_queues;i++)
    {
        queue_post(all_queues[i], id, data);
    }
   
    return num_queues;
}

/****************************************************************************
 * Timer tick
 ****************************************************************************/
#if CONFIG_CPU == SH7034
void tick_start(unsigned int interval_in_ms)
{
    unsigned long count;

    count = CPU_FREQ * interval_in_ms / 1000 / 8;

    if(count > 0x10000)
    {
        panicf("Error! The tick interval is too long (%d ms)\n",
               interval_in_ms);
        return;
    }
    
    /* We are using timer 0 */
    
    TSTR &= ~0x01; /* Stop the timer */
    TSNC &= ~0x01; /* No synchronization */
    TMDR &= ~0x01; /* Operate normally */

    TCNT0 = 0;   /* Start counting at 0 */
    GRA0 = (unsigned short)(count - 1);
    TCR0 = 0x23; /* Clear at GRA match, sysclock/8 */

    /* Enable interrupt on level 1 */
    IPRC = (IPRC & ~0x00f0) | 0x0010;
    
    TSR0 &= ~0x01;
    TIER0 = 0xf9; /* Enable GRA match interrupt */

    TSTR |= 0x01; /* Start timer 1 */
}

void IMIA0(void) __attribute__ ((interrupt_handler));
void IMIA0(void)
{
    int i;

    /* Run through the list of tick tasks */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i])
        {
            tick_funcs[i]();
        }
    }

    current_tick++;

    TSR0 &= ~0x01;
}
#elif defined(CPU_COLDFIRE)
void tick_start(unsigned int interval_in_ms)
{
    unsigned long count;
    int prescale;

    count = CPU_FREQ/2 * interval_in_ms / 1000 / 16;

    if(count > 0x10000)
    {
        panicf("Error! The tick interval is too long (%d ms)\n",
               interval_in_ms);
        return;
    }

    prescale = cpu_frequency / CPU_FREQ;
    /* Note: The prescaler is later adjusted on-the-fly on CPU frequency
       changes within timer.c */
    
    /* We are using timer 0 */

    TRR0 = (unsigned short)(count - 1); /* The reference count */
    TCN0 = 0; /* reset the timer */
    TMR0 = 0x001d | ((unsigned short)(prescale - 1) << 8); 
           /* restart, CLK/16, enabled, prescaler */

    TER0 = 0xff; /* Clear all events */

    ICR1 = 0x8c; /* Interrupt on level 3.0 */
    IMR &= ~0x200;
}

void TIMER0(void) __attribute__ ((interrupt_handler));
void TIMER0(void)
{
    int i;

    /* Run through the list of tick tasks */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i])
        {
            tick_funcs[i]();
        }
    }

    current_tick++;

    TER0 = 0xff; /* Clear all events */
}

#elif defined(CPU_PP)

#ifndef BOOTLOADER
void TIMER1(void)
{
    int i;

    TIMER1_VAL; /* Read value to ack IRQ */

    /* Run through the list of tick tasks (using main core - 
       COP does not dispatch ticks to this subroutine) */
    for (i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if (tick_funcs[i])
        {
            tick_funcs[i]();
        }
    }

    current_tick++;
}
#endif

/* Must be last function called init kernel/thread initialization */
void tick_start(unsigned int interval_in_ms)
{
#ifndef BOOTLOADER
    if(CURRENT_CORE == CPU)
    {
        TIMER1_CFG = 0x0;
        TIMER1_VAL;
        /* enable timer */
        TIMER1_CFG = 0xc0000000 | (interval_in_ms*1000 - 1);
        /* unmask interrupt source */
        CPU_INT_EN = TIMER1_MASK;
    } else {
        COP_INT_EN = TIMER1_MASK;
    }
#else
    /* We don't enable interrupts in the bootloader */
    (void)interval_in_ms;
#endif
}

#elif CONFIG_CPU == PNX0101

void timer_handler(void)
{
    int i;

    /* Run through the list of tick tasks */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i])
            tick_funcs[i]();
    }

    current_tick++;

    TIMER0.clr = 0;
}

void tick_start(unsigned int interval_in_ms)
{
    TIMER0.ctrl &= ~0x80; /* Disable the counter */
    TIMER0.ctrl |= 0x40;  /* Reload after counting down to zero */
    TIMER0.load = 3000000 * interval_in_ms / 1000;
    TIMER0.ctrl &= ~0xc;  /* No prescaler */
    TIMER0.clr = 1;       /* Clear the interrupt request */

    irq_set_int_handler(IRQ_TIMER0, timer_handler);
    irq_enable_int(IRQ_TIMER0);

    TIMER0.ctrl |= 0x80;  /* Enable the counter */
}
#elif CONFIG_CPU == IMX31L
void tick_start(unsigned int interval_in_ms)
{
    EPITCR1 &= ~0x1;        /* Disable the counter */

    EPITCR1 &= ~0xE;        /* Disable interrupt, count down from 0xFFFFFFFF */
    EPITCR1 &= ~0xFFF0;     /* Clear prescaler */
#ifdef BOOTLOADER
    EPITCR1 |= (2700 << 2); /* Prescaler = 2700 */
#endif
    EPITCR1 &= ~(0x3 << 24);
    EPITCR1 |= (0x2 << 24); /* Set clock source to external clock (27mhz) */
    EPITSR1 = 1;            /* Clear the interrupt request */
#ifndef BOOTLOADER
    EPITLR1 = 27000000 * interval_in_ms / 1000;
    EPITCMPR1 = 27000000 * interval_in_ms / 1000;
#else
    (void)interval_in_ms;
#endif

    //avic_enable_int(EPIT1, IRQ, EPIT_HANDLER);

    EPITCR1 |= 0x1;           /* Enable the counter */
}

#ifndef BOOTLOADER
void EPIT_HANDLER(void) __attribute__((interrupt("IRQ")));
void EPIT_HANDLER(void) {
    int i;

    /* Run through the list of tick tasks */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i])
            tick_funcs[i]();
    }

    current_tick++;

    EPITSR1 = 1;            /* Clear the interrupt request */
}
#endif
#endif

int tick_add_task(void (*f)(void))
{
    int i;
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    /* Add a task if there is room */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i] == NULL)
        {
            tick_funcs[i] = f;
            set_irq_level(oldlevel);
            return 0;
        }
    }
    set_irq_level(oldlevel);
    panicf("Error! tick_add_task(): out of tasks");
    return -1;
}

int tick_remove_task(void (*f)(void))
{
    int i;
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    /* Remove a task if it is there */
    for(i = 0;i < MAX_NUM_TICK_TASKS;i++)
    {
        if(tick_funcs[i] == f)
        {
            tick_funcs[i] = NULL;
            set_irq_level(oldlevel);
            return 0;
        }
    }
    
    set_irq_level(oldlevel);
    return -1;
}

/****************************************************************************
 * Tick-based interval timers/one-shots - be mindful this is not really
 * intended for continuous timers but for events that need to run for a short
 * time and be cancelled without further software intervention.
 ****************************************************************************/
#ifdef INCLUDE_TIMEOUT_API
static struct timeout *tmo_list = NULL; /* list of active timeout events */

/* timeout tick task - calls event handlers when they expire
 * Event handlers may alter ticks, callback and data during operation.
 */
static void timeout_tick(void)
{
    unsigned long tick = current_tick;
    struct timeout *curr, *next;

    for (curr = tmo_list; curr != NULL; curr = next)
    {
        next = (struct timeout *)curr->next;

        if (TIME_BEFORE(tick, curr->expires))
            continue;

        /* this event has expired - call callback */
        if (curr->callback(curr))
            *(long *)&curr->expires = tick + curr->ticks; /* reload */
        else
            timeout_cancel(curr); /* cancel */
    }
}

/* Cancels a timeout callback - can be called from the ISR */
void timeout_cancel(struct timeout *tmo)
{
    int oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    if (tmo_list != NULL)
    {
        struct timeout *curr = tmo_list;
        struct timeout *prev = NULL;

        while (curr != tmo && curr != NULL)
        {
            prev = curr;
            curr = (struct timeout *)curr->next;
        }

        if (curr != NULL)
        {
            /* in list */
            if (prev == NULL)
                tmo_list = (struct timeout *)curr->next;
            else
                *(const struct timeout **)&prev->next = curr->next;

            if (tmo_list == NULL)
                tick_remove_task(timeout_tick); /* last one - remove task */
        }
        /* not in list or tmo == NULL */
    }

    set_irq_level(oldlevel);
}

/* Adds a timeout callback - calling with an active timeout resets the
   interval - can be called from the ISR */
void timeout_register(struct timeout *tmo, timeout_cb_type callback,
                      int ticks, intptr_t data)
{
    int oldlevel;
    struct timeout *curr;

    if (tmo == NULL)
        return;

    oldlevel = set_irq_level(HIGHEST_IRQ_LEVEL);

    /* see if this one is already registered */
    curr = tmo_list;
    while (curr != tmo && curr != NULL)
        curr = (struct timeout *)curr->next;

    if (curr == NULL)
    {
        /* not found - add it */
        if (tmo_list == NULL)
            tick_add_task(timeout_tick); /* first one - add task */

        *(struct timeout **)&tmo->next = tmo_list;
        tmo_list = tmo;
    }

    tmo->callback = callback;
    tmo->ticks = ticks;
    tmo->data = data;
    *(long *)&tmo->expires = current_tick + ticks;

    set_irq_level(oldlevel);
}

#endif /* INCLUDE_TIMEOUT_API */

#ifndef SIMULATOR
/*
 * Simulator versions in uisimulator/SIMVER/
 */

/****************************************************************************
 * Simple mutex functions
 ****************************************************************************/
void mutex_init(struct mutex *m)
{
    m->locked = false;
    m->thread = NULL;
}

void mutex_lock(struct mutex *m)
{
    if (test_and_set(&m->locked, 1))
    {
        /* Wait until the lock is open... */
        block_thread(&m->thread);
    }
}

void mutex_unlock(struct mutex *m)
{
    if (m->thread == NULL)
        m->locked = 0;
    else
        wakeup_thread(&m->thread);
}

void spinlock_lock(struct mutex *m)
{
    while (test_and_set(&m->locked, 1))
    {
        /* wait until the lock is open... */
        switch_thread(true, NULL);
    }
}

void spinlock_unlock(struct mutex *m)
{
    m->locked = 0;
}

#endif /* ndef SIMULATOR */
