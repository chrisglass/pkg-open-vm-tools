/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * vmciEvent.c --
 *
 *     VMCI Event code for host and guests.
 */

#if defined(__linux__) && !defined(VMKERNEL)
#  include "driver-config.h"
#  include "compat_kernel.h"
#  include "compat_module.h"
#endif // __linux__
#include "vmci_defs.h"
#include "vmci_kernel_if.h"
#include "vmci_infrastructure.h"
#include "vmciEvent.h"
#ifdef VMX86_TOOLS
#  include "vmciInt.h"
#  include "vmciGuestKernelAPI.h"
#  include "vmciUtil.h"
#elif defined(VMKERNEL)
#  include "vmciVmkInt.h"
#  include "vm_libc.h"
#  include "helper_ext.h"
#  include "vmciDriver.h"
#else
#  include "vmciDriver.h"
#  include "vmciHostKernelAPI.h"
#endif
#include "circList.h"  /* Must come after vmciVmkInt.h. */

#define EVENT_MAGIC 0xEABE0000


typedef struct VMCISubscription {
   VMCIId         id;
   int            refCount;
   Bool           runDelayed;
   VMCIEvent      destroyEvent;
   VMCI_Event     event;
   VMCI_EventCB   callback;
   void           *callbackData;
   ListItem       subscriberListItem;
} VMCISubscription;

typedef struct VMCISubscriptionItem {
   ListItem          listItem;
   VMCISubscription  sub;
} VMCISubscriptionItem;


static VMCISubscription *VMCIEventFind(VMCIId subID);
static int VMCIEventDeliver(VMCIEventMsg *eventMsg);
static int VMCIEventRegisterSubscription(VMCISubscription *sub,
                                         VMCI_Event event,
                                         uint32 flags,
                                         VMCI_EventCB callback,
                                         void *callbackData);
static VMCISubscription *VMCIEventUnregisterSubscription(VMCIId subID);

/*
 * In the guest, VMCI events are dispatched from interrupt context, so
 * the locks need to be bottom half safe. In the host kernel, this
 * isn't so, and regular locks are used instead.
 */

#ifdef VMX86_TOOLS
#define VMCIEventInitLock(_lock, _name) VMCI_InitLock(_lock, _name, VMCI_LOCK_RANK_HIGHER_BH)
#define VMCIEventGrabLock(_lock, _flags) VMCI_GrabLock_BH(_lock, _flags)
#define VMCIEventReleaseLock(_lock, _flags) VMCI_ReleaseLock_BH(_lock, _flags)
#else
#define VMCIEventInitLock(_lock, _name) VMCI_InitLock(_lock, _name, VMCI_LOCK_RANK_HIGHER)
#define VMCIEventGrabLock(_lock, _flags) VMCI_GrabLock(_lock, _flags)
#define VMCIEventReleaseLock(_lock, _flags) VMCI_ReleaseLock(_lock, _flags)
#endif


static ListItem *subscriberArray[VMCI_EVENT_MAX] = {NULL};
static VMCILock subscriberLock;

typedef struct VMCIDelayedEventInfo {
   VMCISubscription *sub;
   uint8 eventPayload[sizeof(VMCIEventData_Max)];
} VMCIDelayedEventInfo;


/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Init --
 *
 *      General init code.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIEvent_Init(void)
{
   VMCIEventInitLock(&subscriberLock, "VMCIEventSubscriberLock");
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Exit --
 *
 *      General exit code.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIEvent_Exit(void)
{
   ListItem *iter, *iter2;
   VMCI_Event e;

   /* We free all memory at exit. */
   for (e = 0; e < VMCI_EVENT_MAX; e++) {
      LIST_SCAN_SAFE(iter, iter2, subscriberArray[e]) {
         VMCISubscription *cur;

         /*
          * We should never get here because all events should have been
          * unregistered before we try to unload the driver module.
          * Also, delayed callbacks could still be firing so this cleanup
          * would not be safe.
          * Still it is better to free the memory than not ... so we
          * leave this code in just in case....
          *
          */
         ASSERT(FALSE);

         cur = LIST_CONTAINER(iter, VMCISubscription, subscriberListItem);
         VMCI_FreeKernelMem(cur, sizeof *cur);
      }
   }
   VMCI_CleanupLock(&subscriberLock);
}

#ifdef VMX86_TOOLS
/*
 *-----------------------------------------------------------------------------
 *
 * VMCIEvent_CheckHostCapabilities --
 *
 *      Verify that the host supports the hypercalls we need. If it does not,
 *      try to find fallback hypercalls and use those instead.
 *
 * Results:
 *      TRUE if required hypercalls (or fallback hypercalls) are
 *      supported by the host, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VMCIEvent_CheckHostCapabilities(void)
{
   /* VMCIEvent does not require any hypercalls. */
   return TRUE;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIEventGet --
 *
 *      Gets a reference to the given VMCISubscription.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
VMCIEventGet(VMCISubscription *entry)  // IN
{
   ASSERT(entry);

   entry->refCount++;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIEventRelease --
 *
 *      Releases the given VMCISubscription.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Fires the destroy event if the reference count has gone to zero.
 *
 *-----------------------------------------------------------------------------
 */

static void
VMCIEventRelease(VMCISubscription *entry)  // IN
{
   ASSERT(entry);
   ASSERT(entry->refCount > 0);

   entry->refCount--;
   if (entry->refCount == 0) {
      VMCI_SignalEvent(&entry->destroyEvent);
   }
}


 /*
 *------------------------------------------------------------------------------
 *
 *  EventReleaseCB --
 *
 *     Callback to release the event entry reference. It is called by the
 *     VMCI_WaitOnEvent function before it blocks.
 *
 *  Result:
 *     None.
 *
 *  Side effects:
 *     None.
 *
 *------------------------------------------------------------------------------
 */

static int
EventReleaseCB(void *clientData) // IN
{
   VMCILockFlags flags;
   VMCISubscription *sub = (VMCISubscription *)clientData;

   ASSERT(sub);

   VMCIEventGrabLock(&subscriberLock, &flags);
   VMCIEventRelease(sub);
   VMCIEventReleaseLock(&subscriberLock, flags);

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIEventFind --
 *
 *      Find entry. Assumes lock is held.
 *
 * Results:
 *      Entry if found, NULL if not.
 *
 * Side effects:
 *      Increments the VMCISubscription refcount if an entry is found.
 *
 *-----------------------------------------------------------------------------
 */

static VMCISubscription *
VMCIEventFind(VMCIId subID)  // IN
{
   ListItem *iter;
   VMCI_Event e;

   for (e = 0; e < VMCI_EVENT_MAX; e++) {
      LIST_SCAN(iter, subscriberArray[e]) {
         VMCISubscription *cur =
            LIST_CONTAINER(iter, VMCISubscription, subscriberListItem);
         if (cur->id == subID) {
            VMCIEventGet(cur);
            return cur;
         }
      }
   }
   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventDelayedDispatchCB --
 *
 *      Calls the specified callback in a delayed context.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
VMCIEventDelayedDispatchCB(void *data) // IN
{
   VMCIDelayedEventInfo *eventInfo;
   VMCISubscription *sub;
   VMCI_EventData *ed;
   VMCILockFlags flags;

   eventInfo = (VMCIDelayedEventInfo *)data;

   ASSERT(eventInfo);
   ASSERT(eventInfo->sub);

   sub = eventInfo->sub;
   ed = (VMCI_EventData *)eventInfo->eventPayload;

   sub->callback(sub->id, ed, sub->callbackData);

   VMCIEventGrabLock(&subscriberLock, &flags);
   VMCIEventRelease(sub);
   VMCIEventReleaseLock(&subscriberLock, flags);

   VMCI_FreeKernelMem(eventInfo, sizeof *eventInfo);
}


/*
 *----------------------------------------------------------------------------
 *
 * VMCIEventDeliver --
 *
 *      Actually delivers the events to the subscribers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The callback function for each subscriber is invoked.
 *
 *----------------------------------------------------------------------------
 */

static int
VMCIEventDeliver(VMCIEventMsg *eventMsg)  // IN
{
   int err = VMCI_SUCCESS;
   ListItem *iter;
   VMCILockFlags flags;

   ASSERT(eventMsg);

   VMCIEventGrabLock(&subscriberLock, &flags);
   LIST_SCAN(iter, subscriberArray[eventMsg->eventData.event]) {
      VMCI_EventData *ed;
      VMCISubscription *cur = LIST_CONTAINER(iter, VMCISubscription,
                                             subscriberListItem);
      ASSERT(cur && cur->event == eventMsg->eventData.event);

      if (cur->runDelayed) {
         VMCIDelayedEventInfo *eventInfo;
         if ((eventInfo = VMCI_AllocKernelMem(sizeof *eventInfo,
                                              (VMCI_MEMORY_ATOMIC |
                                               VMCI_MEMORY_NONPAGED))) == NULL) {
            err = VMCI_ERROR_NO_MEM;
            goto out;
         }

         VMCIEventGet(cur);

         memset(eventInfo, 0, sizeof *eventInfo);
         memcpy(eventInfo->eventPayload, VMCI_DG_PAYLOAD(eventMsg),
                (size_t)eventMsg->hdr.payloadSize);
         eventInfo->sub = cur;
         err = VMCI_ScheduleDelayedWork(VMCIEventDelayedDispatchCB,
                                        eventInfo);
         if (err != VMCI_SUCCESS) {
            VMCIEventRelease(cur);
            VMCI_FreeKernelMem(eventInfo, sizeof *eventInfo);
            goto out;
         }

      } else {
         uint8 eventPayload[sizeof(VMCIEventData_Max)];

         /* We set event data before each callback to ensure isolation. */
         memset(eventPayload, 0, sizeof eventPayload);
         memcpy(eventPayload, VMCI_DG_PAYLOAD(eventMsg),
                (size_t)eventMsg->hdr.payloadSize);
         ed = (VMCI_EventData *)eventPayload;
         cur->callback(cur->id, ed, cur->callbackData);
      }
   }

out:
   VMCIEventReleaseLock(&subscriberLock, flags);

   return err;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Dispatch --
 *
 *      Dispatcher for the VMCI_EVENT_RECEIVE datagrams. Calls all
 *      subscribers for given event.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIEvent_Dispatch(VMCIDatagram *msg)  // IN
{
   VMCIEventMsg *eventMsg = (VMCIEventMsg *)msg;

   ASSERT(msg &&
          msg->src.context == VMCI_HYPERVISOR_CONTEXT_ID &&
          msg->dst.resource == VMCI_EVENT_HANDLER);

   if (msg->payloadSize < sizeof(VMCI_Event) ||
       msg->payloadSize > sizeof(VMCIEventData_Max)) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (eventMsg->eventData.event >= VMCI_EVENT_MAX) {
      return VMCI_ERROR_EVENT_UNKNOWN;
   }

   VMCIEventDeliver(eventMsg);

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventRegisterSubscription --
 *
 *      Initialize and add subscription to subscriber list.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VMCIEventRegisterSubscription(VMCISubscription *sub,   // IN
                              VMCI_Event event,        // IN
                              uint32 flags,            // IN
                              VMCI_EventCB callback,   // IN
                              void *callbackData)      // IN
{
#  define VMCI_EVENT_MAX_ATTEMPTS 10
   static VMCIId subscriptionID = 0;
   VMCILockFlags lockFlags;
   uint32 attempts = 0;
   int result;
   Bool success;

   ASSERT(sub);

   if (event >= VMCI_EVENT_MAX || callback == NULL) {
      VMCILOG(("VMCIEvent: Failed to subscribe to event %d cb %p data %p.\n",
               event, callback, callbackData));
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (vmkernel) {
      /*
       * In the vmkernel we defer delivery of events to a helper world.  This
       * makes the event delivery more consistent across hosts and guests with
       * regard to which locks are held.
       */
      sub->runDelayed = TRUE;
   } else if (!VMCI_CanScheduleDelayedWork()) {
      /*
       * If the platform doesn't support delayed work callbacks then don't
       * allow registration for them.
       */
      if (flags & VMCI_FLAG_EVENT_DELAYED_CB) {
         return VMCI_ERROR_INVALID_ARGS;
      }
      sub->runDelayed = FALSE;
   } else {
      /*
       * The platform supports delayed work callbacks. Honor the requested
       * flags
       */
      sub->runDelayed = (flags & VMCI_FLAG_EVENT_DELAYED_CB) ? TRUE : FALSE;
   }

   sub->refCount = 1;
   sub->event = event;
   sub->callback = callback;
   sub->callbackData = callbackData;

   VMCIEventGrabLock(&subscriberLock, &lockFlags);
   for (success = FALSE, attempts = 0;
        success == FALSE && attempts < VMCI_EVENT_MAX_ATTEMPTS;
        attempts++) {
      VMCISubscription *existingSub = NULL;

      /*
       * We try to get an id a couple of time before claiming we are out of
       * resources.
       */
      sub->id = ++subscriptionID;

      /* Test for duplicate id. */
      existingSub = VMCIEventFind(sub->id);
      if (existingSub == NULL) {
         /* We succeeded if we didn't find a duplicate. */
         success = TRUE;
      } else {
         VMCIEventRelease(existingSub);
      }
   }

   if (success) {
      VMCI_CreateEvent(&sub->destroyEvent);
      LIST_QUEUE(&sub->subscriberListItem, &subscriberArray[event]);
      result = VMCI_SUCCESS;
   } else {
      result = VMCI_ERROR_NO_RESOURCES;
   }
   VMCIEventReleaseLock(&subscriberLock, lockFlags);

   return result;
#  undef VMCI_EVENT_MAX_ATTEMPTS
}



/*
 *----------------------------------------------------------------------
 *
 * VMCIEventUnregisterSubscription --
 *
 *      Remove subscription from subscriber list.
 *
 * Results:
 *      VMCISubscription when found, NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMCISubscription *
VMCIEventUnregisterSubscription(VMCIId subID)    // IN
{
   VMCILockFlags flags;
   VMCISubscription *s;

   VMCIEventGrabLock(&subscriberLock, &flags);
   s = VMCIEventFind(subID);
   if (s != NULL) {
      VMCIEventRelease(s);
      LIST_DEL(&s->subscriberListItem, &subscriberArray[s->event]);
   }
   VMCIEventReleaseLock(&subscriberLock, flags);

   if (s != NULL) {
      VMCI_WaitOnEvent(&s->destroyEvent, EventReleaseCB, s);
      VMCI_DestroyEvent(&s->destroyEvent);
   }

   return s;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventSubscribe --
 *
 *      Subscribe to given event. The callback specified can be fired
 *      in different contexts depending on what flag is specified while
 *      registering. If flags contains VMCI_FLAG_EVENT_NONE then the
 *      callback is fired with the subscriber lock held (and BH context
 *      on the guest). If flags contain VMCI_FLAG_EVENT_DELAYED_CB then
 *      the callback is fired with no locks held in thread context.
 *      This is useful because other VMCIEvent functions can be called,
 *      but it also increases the chances that an event will be dropped.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIEventSubscribe(VMCI_Event event,        // IN
                   uint32 flags,            // IN
                   VMCI_EventCB callback,   // IN
                   void *callbackData,      // IN
                   VMCIId *subscriptionID)  // OUT
{
   int retval;
   VMCISubscription *s = NULL;

   if (subscriptionID == NULL) {
      VMCILOG(("VMCIEvent: Invalid arguments.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   s = VMCI_AllocKernelMem(sizeof *s, VMCI_MEMORY_NONPAGED);
   if (s == NULL) {
      return VMCI_ERROR_NO_MEM;
   }

   retval = VMCIEventRegisterSubscription(s, event, flags,
                                          callback, callbackData);
   if (retval < VMCI_SUCCESS) {
      VMCI_FreeKernelMem(s, sizeof *s);
      return retval;
   }

   *subscriptionID = s->id;
   return retval;
}


#ifndef VMKERNEL
/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Subscribe --
 *
 *      Subscribe to given event.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#if defined(__linux__)
EXPORT_SYMBOL(VMCIEvent_Subscribe);
#endif

int
VMCIEvent_Subscribe(VMCI_Event event,        // IN
                    uint32 flags,            // IN
                    VMCI_EventCB callback,   // IN
                    void *callbackData,      // IN
                    VMCIId *subscriptionID)  // OUT
{
   return VMCIEventSubscribe(event, flags, callback, callbackData,
                             subscriptionID);
}
#endif	/* !VMKERNEL  */


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventUnsubscribe --
 *
 *      Unsubscribe to given event. Removes it from list and frees it.
 *      Will return callbackData if requested by caller.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIEventUnsubscribe(VMCIId subID)   // IN
{
   VMCISubscription *s;

   /*
    * Return subscription. At this point we know noone else is accessing
    * the subscription so we can free it.
    */
   s = VMCIEventUnregisterSubscription(subID);
   if (s == NULL) {
      return VMCI_ERROR_NOT_FOUND;

   }
   VMCI_FreeKernelMem(s, sizeof *s);

   return VMCI_SUCCESS;
}


#ifndef VMKERNEL
/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Unsubscribe --
 *
 *      Unsubscribe to given event. Removes it from list and frees it.
 *      Will return callbackData if requested by caller.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#if defined(__linux__)
EXPORT_SYMBOL(VMCIEvent_Unsubscribe);
#endif

int
VMCIEvent_Unsubscribe(VMCIId subID)   // IN
{
   return VMCIEventUnsubscribe(subID);
}

#endif /* !VMKERNEL  */
