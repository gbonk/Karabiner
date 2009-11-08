#include "Core.hpp"
#include "Config.hpp"
#include "RemapUtil.hpp"
#include "Client.hpp"
#include "util/ListHookedKeyboard.hpp"
#include "util/ListHookedConsumer.hpp"
#include "util/ListHookedPointing.hpp"
#include "util/TimerWrapper.hpp"
#include "util/NumHeldDownKeys.hpp"
#include "remap.hpp"
#include "bridge.hpp"

#include <sys/errno.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>

namespace org_pqrs_KeyRemap4MacBook {
  namespace Core {
    namespace {
      IOWorkLoop *workLoop = NULL;
      TimerWrapper timer_refresh;

      TimerWrapper timer_repeat_keyboard;
      TimerWrapper timer_repeat_keyboard_extra;
      TimerWrapper timer_repeat_consumer;

      class KeyboardRepeatInfo {
      public:
        KeyboardRepeatInfo(void) { kbd = NULL; }
        const IOHIKeyboard *kbd;
        Params_KeyboardEventCallBack params;
      } keyboardRepeatInfo;

      class KeyboardRepeatInfo_extra {
      public:
        KeyboardRepeatInfo_extra(void) { kbd = NULL; }
        IOHIKeyboard *kbd;
        Params_KeyboardEventCallBack params;
        ExtraRepeatFunc::ExtraRepeatFunc func;
        unsigned int counter;
      } keyboardRepeatInfo_extra;

      // ------------------------------------------------------------
      enum {
        REFRESH_DEVICE_INTERVAL = 3000,
      };

      void
      refreshHookedDevice(OSObject *owner, IOTimerEventSource *sender)
      {
        ListHookedKeyboard::instance().refresh();
        ListHookedConsumer::instance().refresh();
        ListHookedPointing::instance().refresh();

        if (sender) sender->setTimeoutMS(REFRESH_DEVICE_INTERVAL);
      }

      // ----------------------------------------
      void
      setRepeat_keyboard(const IOHIKeyboard *kbd, const Params_KeyboardEventCallBack &params)
      {
        if (params.eventType == KeyEvent::DOWN) {
          keyboardRepeatInfo.kbd = kbd;
          keyboardRepeatInfo.params = params;

          IOReturn result = timer_repeat_keyboard.setTimeoutMS(config.get_repeat_initial_wait());
          if (result != kIOReturnSuccess) {
            IOLog("[KeyRemap4MacBook ERROR] setTimeoutMS failed\n");
          }

        } else if (params.eventType == KeyEvent::MODIFY || keyboardRepeatInfo.params.key == params.key) {
          timer_repeat_keyboard.cancelTimeout();
        }
      }

      void
      setRepeat_keyboard_extra(IOHIKeyboard *kbd, const Params_KeyboardEventCallBack &params, ExtraRepeatFunc::ExtraRepeatFunc func, unsigned int flags)
      {
        if (func) {
          keyboardRepeatInfo_extra.kbd = kbd;
          keyboardRepeatInfo_extra.func = func;
          keyboardRepeatInfo_extra.counter = 0;
          keyboardRepeatInfo_extra.params = params;
          keyboardRepeatInfo_extra.params.flags = flags;

          timer_repeat_keyboard_extra.setTimeoutMS(config.get_keyoverlaidmodifier_initial_wait());

        } else {
          timer_repeat_keyboard_extra.cancelTimeout();
          keyboardRepeatInfo_extra.func = NULL;
        }
      }

      void
      doRepeat_keyboard(OSObject *owner, IOTimerEventSource *sender)
      {
        HookedKeyboard *p = ListHookedKeyboard::instance().get(keyboardRepeatInfo.kbd);
        if (! p) return;

        RemapUtil::fireKey(p->getOrig_keyboardEventAction(), keyboardRepeatInfo.params);
        sender->setTimeoutMS(config.get_repeat_wait());
      }

      void
      doRepeat_keyboard_extra(OSObject *owner, IOTimerEventSource *sender)
      {
        HookedKeyboard *p = ListHookedKeyboard::instance().get(keyboardRepeatInfo_extra.kbd);
        if (! p) return;

        if (p->getOrig_keyboardEventAction()) {
          keyboardRepeatInfo_extra.func(p->getOrig_keyboardEventAction(),
                                        keyboardRepeatInfo_extra.params.target,
                                        keyboardRepeatInfo_extra.params.flags,
                                        keyboardRepeatInfo_extra.params.keyboardType,
                                        keyboardRepeatInfo_extra.params.ts,
                                        keyboardRepeatInfo_extra.params.sender,
                                        keyboardRepeatInfo_extra.params.refcon);
        }
        ++(keyboardRepeatInfo_extra.counter);

        sender->setTimeoutMS(config.get_repeat_wait());
      }
    }

    void
    initialize(void)
    {
      sysctl_register();
      ClickWatcher::reset();
      PressDownKeys::initialize();
      FlagStatus::initialize();
      KeyRemap4MacBook_client::initialize();
    }

    void
    terminate(void)
    {
      KeyRemap4MacBook_client::terminate();
      sysctl_unregister();
    }

    void
    start(void)
    {
      ListHookedKeyboard::instance().initialize();
      ListHookedConsumer::instance().initialize();
      ListHookedPointing::instance().initialize();

      workLoop = IOWorkLoop::workLoop();
      if (! workLoop) {
        IOLog("[KeyRemap4MacBook ERROR] IOWorkLoop::workLoop failed\n");
      } else {
        timer_refresh.initialize(workLoop, NULL, refreshHookedDevice);
        timer_refresh.setTimeoutMS(REFRESH_DEVICE_INTERVAL);

        timer_repeat_keyboard.initialize(workLoop, NULL, doRepeat_keyboard);
        timer_repeat_keyboard_extra.initialize(workLoop, NULL, doRepeat_keyboard_extra);
      }
    }

    void
    stop(void)
    {
      timer_refresh.terminate();
      ListHookedKeyboard::instance().terminate();
      ListHookedConsumer::instance().terminate();
      ListHookedPointing::instance().terminate();

      if (workLoop) {
        workLoop->release();
        workLoop = NULL;
      }
    }

    // ======================================================================
    bool
    notifierfunc_hookKeyboard(void *target, void *refCon, IOService *newService, IONotifier* notifier)
    {
      IOLog("KeyRemap4MacBook notifierfunc_hookKeyboard\n");

      IOHIKeyboard *kbd = OSDynamicCast(IOHIKeyboard, newService);
      ListHookedKeyboard::instance().append(kbd);
      ListHookedConsumer::instance().append(kbd);
      return true;
    }

    bool
    notifierfunc_unhookKeyboard(void *target, void *refCon, IOService *newService, IONotifier* notifier)
    {
      IOLog("KeyRemap4MacBook notifierfunc_unhookKeyboard\n");

      IOHIKeyboard *kbd = OSDynamicCast(IOHIKeyboard, newService);
      ListHookedKeyboard::instance().terminate(kbd);
      ListHookedConsumer::instance().terminate(kbd);
      return true;
    }

    bool
    notifierfunc_hookPointing(void *target, void *refCon, IOService *newService, IONotifier* notifier)
    {
      IOLog("KeyRemap4MacBook notifierfunc_hookPointing\n");

      IOHIPointing *pointing = OSDynamicCast(IOHIPointing, newService);
      return ListHookedPointing::instance().append(pointing);
    }

    bool
    notifierfunc_unhookPointing(void *target, void *refCon, IOService *newService, IONotifier* notifier)
    {
      IOLog("KeyRemap4MacBook notifierfunc_unhookPointing\n");

      IOHIPointing *pointing = OSDynamicCast(IOHIPointing, newService);
      return ListHookedPointing::instance().terminate(pointing);
    }

    // ======================================================================
    void
    remap_KeyboardEventCallback(Params_KeyboardEventCallBack *params)
    {
      if (! params) return;

      IOHIKeyboard *kbd = OSDynamicCast(IOHIKeyboard, params->sender);
      if (! kbd) return;

      HookedKeyboard *p = ListHookedKeyboard::instance().get(kbd);
      if (! p) return;

      // ------------------------------------------------------------
      // Because the key repeat generates it by oneself, I throw it away.
      if (params->repeat) {
        keyboardRepeatInfo.params.ts = params->ts;
        keyboardRepeatInfo_extra.params.ts = params->ts;
        return;
      }

      params->log();

      // ------------------------------------------------------------
      if (config.general_capslock_led_hack) {
        int led = kbd->getLEDStatus();
        if (led == 0) {
          kbd->setAlphaLockFeedback(true);
        }
      }

      // ------------------------------------------------------------
      ListFireExtraKey::reset();
      listFireConsumerKey.reset();
      listFireRelativePointer.reset();

      unsigned int ex_extraRepeatFlags = 0;
      ExtraRepeatFunc::ExtraRepeatFunc ex_extraRepeatFunc = NULL;

      KeyCode::normalizeKey(params->key, params->flags, params->keyboardType);

      RemapParams remapParams = {
        params,
        params->key,
        KeyRemap4MacBook_bridge::GetWorkspaceData::UNKNOWN,
        KeyRemap4MacBook_bridge::GetWorkspaceData::INPUTMODE_ROMAN,
        KeyRemap4MacBook_bridge::GetWorkspaceData::INPUTMODE_DETAIL_ROMAN,
        &ex_extraRepeatFunc,
        &ex_extraRepeatFlags,
        keyboardRepeatInfo_extra.counter,
      };
      NumHeldDownKeys::set(remapParams);

      // ------------------------------------------------------------
      /*
        When we press the functional key (ex. F2) with the keyboard of the third vendor,
        KeyRemap4MacBook_client::sendmsg returns EIO.

        We use the previous value when the error occurred.
      */
      static KeyRemap4MacBook_bridge::GetWorkspaceData::ApplicationType lastApplicationType = KeyRemap4MacBook_bridge::GetWorkspaceData::UNKNOWN;
      static KeyRemap4MacBook_bridge::GetWorkspaceData::InputMode lastInputMode = KeyRemap4MacBook_bridge::GetWorkspaceData::INPUTMODE_ROMAN;
      static KeyRemap4MacBook_bridge::GetWorkspaceData::InputModeDetail lastInputModeDetail = KeyRemap4MacBook_bridge::GetWorkspaceData::INPUTMODE_DETAIL_ROMAN;

      KeyRemap4MacBook_bridge::GetWorkspaceData::Reply workspacedata;
      int error = KeyRemap4MacBook_client::sendmsg(KeyRemap4MacBook_bridge::REQUEST_GET_WORKSPACE_DATA, NULL, 0, &workspacedata, sizeof(workspacedata));
      if (config.debug_devel) {
        printf("KeyRemap4MacBook -Info- GetWorkspaceData: %d,%d,%d (error: %d)\n", workspacedata.type, workspacedata.inputmode, workspacedata.inputmodedetail, error);
      }
      if (error == 0) {
        remapParams.appType = workspacedata.type;
        lastApplicationType = workspacedata.type;

        remapParams.inputmode = workspacedata.inputmode;
        lastInputMode = workspacedata.inputmode;

        remapParams.inputmodedetail = workspacedata.inputmodedetail;
        lastInputModeDetail = workspacedata.inputmodedetail;
      } else {
        // use last info.
        remapParams.appType = lastApplicationType;
        remapParams.inputmode = lastInputMode;
        remapParams.inputmodedetail = lastInputModeDetail;
      }

      // ------------------------------------------------------------
      remap_core(remapParams);

      // ------------------------------------------------------------
      // pointing emulation
      if (! listFireRelativePointer.isEmpty()) {
        HookedPointing *hp = ListHookedPointing::instance().get();
        if (hp) {
          listFireRelativePointer.fire(hp->getOrig_relativePointerEventAction(), hp->getOrig_relativePointerEventTarget(), hp->get(), params->ts);
        }
      }

      // consumer emulation
      if (! listFireConsumerKey.isEmpty()) {
        HookedConsumer *hc = ListHookedConsumer::instance().get();
        if (hc) {
          listFireConsumerKey.fire(hc->getOrig_keyboardSpecialEventAction(), hc->getOrig_keyboardSpecialEventTarget(), params->ts, params->sender, params->refcon);
        }
      }

      RemapUtil::fireKey(p->getOrig_keyboardEventAction(), *params);
      ListFireExtraKey::fire(p->getOrig_keyboardEventAction(), *params);

      setRepeat_keyboard(kbd, *params);
      setRepeat_keyboard_extra(kbd, *params, ex_extraRepeatFunc, ex_extraRepeatFlags);

      if (NumHeldDownKeys::iszero()) {
        NumHeldDownKeys::reset();
        FlagStatus::reset();
        params->flags = FlagStatus::makeFlags(params->key);
        RemapUtil::fireModifiers(p->getOrig_keyboardEventAction(), *params);
        PressDownKeys::clear(p->getOrig_keyboardEventAction(), params->target, params->ts, params->sender, params->refcon);
      }
    }

    void
    remap_KeyboardSpecialEventCallback(Params_KeyboardSpecialEventCallback *params)
    {
      if (! params) return;

      IOHIKeyboard *kbd = OSDynamicCast(IOHIKeyboard, params->sender);
      if (! kbd) return;

      HookedConsumer *p = ListHookedConsumer::instance().get(kbd);
      if (! p) return;

      // ------------------------------------------------------------
      params->log();

      ListFireExtraKey::reset();
      KeyCode::KeyCode ex_remapKeyCode = KeyCode::NONE;
      RemapConsumerParams remapParams = {
        params, &ex_remapKeyCode,
      };
      NumHeldDownKeys::set(remapParams);
      remap_consumer(remapParams);

      // ----------------------------------------
      HookedKeyboard *hk = ListHookedKeyboard::instance().get();
      unsigned int keyboardType = KeyboardType::MACBOOK;

      if (hk) {
        Params_KeyboardEventCallBack callbackparams = {
          hk->getOrig_keyboardEventTarget(), params->eventType, params->flags, ex_remapKeyCode,
          0, 0, 0, 0,
          keyboardType, false, params->ts, hk->get(), NULL,
        };

        if (ex_remapKeyCode != KeyCode::NONE) {
          RemapUtil::fireKey(hk->getOrig_keyboardEventAction(), callbackparams);
          setRepeat_keyboard(hk->get(), callbackparams);
        }
        ListFireExtraKey::fire(hk->getOrig_keyboardEventAction(), callbackparams);
      }

      RemapUtil::fireConsumer(p->getOrig_keyboardSpecialEventAction(), *params);
    }

    void
    remap_RelativePointerEventCallback(Params_RelativePointerEventCallback *params)
    {
      if (! params) return;

      IOHIPointing *pointing = OSDynamicCast(IOHIPointing, params->sender);
      if (! pointing) return;

      HookedPointing *p = ListHookedPointing::instance().get(pointing);
      if (! p) return;

      // ------------------------------------------------------------
      params->log();

      listFireRelativePointer.reset();
      unsigned int flags = FlagStatus::makeFlags(KeyCode::NONE);

      bool ex_dropEvent = false;
      RemapPointingParams_relative remapParams = {
        params, &ex_dropEvent,
      };
      remap_pointing_relative_core(remapParams);

      // ------------------------------------------------------------
      unsigned int newflags = FlagStatus::makeFlags(KeyCode::NONE);
      if (flags != newflags) {
        HookedKeyboard *hk = ListHookedKeyboard::instance().get();
        unsigned int keyboardType = KeyboardType::MACBOOK;
        if (hk) {
          Params_KeyboardEventCallBack callbackparams = {
            hk->getOrig_keyboardEventTarget(), KeyEvent::MODIFY, newflags, KeyCode::NONE,
            0, 0, 0, 0,
            keyboardType, false, params->ts, hk->get(), NULL,
          };
          RemapUtil::fireModifiers(hk->getOrig_keyboardEventAction(), callbackparams);
        }
      }

      // ------------------------------------------------------------
      RelativePointerEventCallback reCallback = p->getOrig_relativePointerEventAction();

      if (! ex_dropEvent) {
        params->apply(reCallback);
      }

      if (! listFireRelativePointer.isEmpty()) {
        listFireRelativePointer.fire(reCallback, params->target, pointing, params->ts);
      }

      if (firePointingScroll.isEnable()) {
        firePointingScroll.fire(p->getOrig_scrollWheelEventAction(), p->getOrig_scrollWheelEventTarget(), pointing, params->ts);
      }
    }

    void
    remap_ScrollWheelEventCallback(Params_ScrollWheelEventCallback *params)
    {
      if (! params) return;

      IOHIPointing *pointing = OSDynamicCast(IOHIPointing, params->sender);
      if (! pointing) return;

      HookedPointing *p = ListHookedPointing::instance().get(pointing);
      if (! p) return;

      // ------------------------------------------------------------
      params->log();

      params->apply(p->getOrig_scrollWheelEventAction());
    }
  }
}
