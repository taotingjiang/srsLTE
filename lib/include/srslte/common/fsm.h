/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#ifndef SRSLTE_FSM_H
#define SRSLTE_FSM_H

#include "choice_type.h"
#include "srslte/common/logmap.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <tuple>

//! Helper to print the name of a type for logging
#if defined(__GNUC__) && !defined(__clang__)
template <typename T>
std::string get_type_name()
{
  static const char* s    = __PRETTY_FUNCTION__;
  static const char* pos1 = strchr(s, '=') + 2;
  return std::string{pos1, strchr(pos1, ';')};
}
#else
template <typename T>
std::string get_type_name()
{
  return "anonymous";
}
#endif

//! This version leverages type deduction. (e.g. get_type_name(var))
template <typename T>
std::string get_type_name(const T& t)
{
  return get_type_name<T>();
}

namespace srslte {

//! When there is no state transition
struct same_state {
};

namespace fsm_details {

//! Visitor to get a state's name string
struct state_name_visitor {
  template <typename State>
  void operator()(State&& s)
  {
    name = get_type_name(s);
  }
  std::string name = "invalid state";
};

struct fsm_helper {
  //! Metafunction to determine if FSM can hold given State type
  template <typename FSM>
  using get_fsm_state_list = decltype(std::declval<typename FSM::derived_view>().states);
  template <typename FSM, typename State>
  using enable_if_fsm_state = typename get_fsm_state_list<FSM>::template enable_if_can_hold<State>;
  template <typename FSM, typename State>
  using disable_if_fsm_state = typename get_fsm_state_list<FSM>::template disable_if_can_hold<State>;

  template <typename FSM>
  static auto call_init(FSM* f) -> decltype(f->derived()->do_init())
  {
    f->derived()->do_init();
  }
  static void call_init(...) {}
  template <typename FSM, typename State>
  static auto call_enter(FSM* f, State* s) -> decltype(f->enter(*s))
  {
    f->enter(*s);
    call_init(s);
  }
  static void call_enter(...) {}
  template <typename FSM, typename State>
  static auto call_exit(FSM* f, State* s) -> decltype(f->exit(*s))
  {
    f->exit(*s);
  }
  static void call_exit(...) {}

  template <typename FSM, typename PrevState>
  struct variant_convert {
    variant_convert(FSM* f_, PrevState* p_) : f(f_), p(p_) {}
    template <typename State>
    void operator()(State& s)
    {
      static_assert(not std::is_same<typename std::decay<State>::type, typename std::decay<PrevState>::type>::value,
                    "State cannot transition to itself.\n");
      call_exit(f, &srslte::get<PrevState>(f->states));
      f->states.transit(std::move(s));
      call_enter(f, &srslte::get<State>(f->states));
    }
    FSM*       f;
    PrevState* p;
  };

  template <typename FSM>
  struct enter_visitor {
    enter_visitor(FSM* f_) : f(f_) {}
    template <typename State>
    void operator()(State&& s)
    {
      call_enter(f, &s);
    }
    FSM* f;
  };

  //! Stayed in same state
  template <typename FSM, typename PrevState>
  static void handle_state_change(FSM* f, same_state* s, PrevState* p)
  {
    // do nothing
  }
  //! TargetState is type-erased (a choice). Apply its stored type to the fsm current state
  template <typename FSM, typename... Args, typename PrevState>
  static void handle_state_change(FSM* f, choice_t<Args...>* s, PrevState* p)
  {
    fsm_details::fsm_helper::variant_convert<FSM, PrevState> visitor{f, p};
    s->visit(visitor);
  }
  //! Simple state transition in FSM
  template <typename FSM, typename State, typename PrevState>
  static enable_if_fsm_state<FSM, State> handle_state_change(FSM* f, State* s, PrevState* p)
  {
    static_assert(not std::is_same<State, PrevState>::value, "State cannot transition to itself.\n");
    call_exit(f, &srslte::get<PrevState>(f->states));
    f->states.transit(std::move(*s));
    call_enter(f, &srslte::get<State>(f->states));
  }
  //! State not present in current FSM. Attempt state transition in parent FSM in the case of NestedFSM
  template <typename FSM, typename State, typename PrevState>
  static disable_if_fsm_state<FSM, State> handle_state_change(FSM* f, State* s, PrevState* p)
  {
    static_assert(FSM::is_nested, "State is not present in the FSM list of valid states");
    if (p != nullptr) {
      //      srslte::get<PrevState>(f->states).do_exit();
      call_exit(f, &srslte::get<PrevState>(f->states));
    }
    handle_state_change(f->parent_fsm()->derived(), s, static_cast<typename FSM::derived_t*>(f));
  }

  //! Trigger Event, that will result in a state transition
  template <typename FSM, typename Event>
  struct trigger_visitor {
    trigger_visitor(FSM* f_, Event&& ev_) : f(f_), ev(std::forward<Event>(ev_)) {}

    //! Trigger visitor callback for the current state
    template <typename CurrentState>
    void operator()(CurrentState& s)
    {
      call_trigger(&s);
    }

    //! Compute next state type
    template <typename State>
    using NextState = decltype(std::declval<FSM>().react(std::declval<State&>(), std::declval<Event>()));

    //! In case a react(CurrentState&, Event) method is found
    template <typename State>
    auto call_trigger(State* current_state) -> NextState<State>
    {
      static_assert(not std::is_same<NextState<State>, State>::value, "State cannot transition to itself.\n");
      auto target_state = f->react(*current_state, std::move(ev));
      fsm_helper::handle_state_change(f, &target_state, current_state);
      return target_state;
    }
    //! No react method found. Try forward trigger to HSM
    template <typename State, typename... Args>
    void call_trigger(State* current_state, Args&&... args)
    {
      call_trigger2(current_state);
    }
    //! In case a react(CurrentState&, Event) method is not found, but we are in a NestedFSM with a trigger method
    template <typename State>
    auto call_trigger2(State* s) -> decltype(std::declval<State>().trigger(std::declval<Event>()))
    {
      s->trigger(std::move(ev));
    }
    //! No trigger or react method found. Do nothing
    void call_trigger2(...) { f->unhandled_event(std::move(ev)); }

    FSM*  f;
    Event ev;
  };
};

} // namespace fsm_details

//! CRTP Class for all non-nested FSMs
template <typename Derived>
class fsm_t
{
protected:
  using base_t = fsm_t<Derived>;
  // get access to derived protected members from the base
  class derived_view : public Derived
  {
  public:
    using derived_t = Derived;
    using Derived::do_init;
    using Derived::enter;
    using Derived::exit;
    using Derived::react;
    using Derived::states;
    using Derived::unhandled_event;
    using Derived::base_t::unhandled_event;
  };

public:
  static const bool is_nested = false;

  template <typename... States>
  struct state_list : public choice_t<States...> {
    using base_t = choice_t<States...>;
    template <typename... Args>
    state_list(fsm_t<Derived>* f, Args&&... args) : base_t(std::forward<Args>(args)...)
    {
      if (not Derived::is_nested) {
        fsm_details::fsm_helper::enter_visitor<derived_view> visitor{f->derived()};
        this->visit(visitor);
      }
    }
    template <typename State>
    void transit(State&& s)
    {
      this->template emplace<State>(std::forward<State>(s));
    }
  };

  explicit fsm_t(srslte::log_ref log_) : log_h(log_) {}

  // Push Events to FSM
  template <typename Ev>
  void trigger(Ev&& e)
  {
    fsm_details::fsm_helper::trigger_visitor<derived_view, Ev> visitor{derived(), std::forward<Ev>(e)};
    derived()->states.visit(visitor);
  }

  template <typename State>
  bool is_in_state() const
  {
    return derived()->states.template is<State>();
  }

  template <typename State>
  const State* get_state() const
  {
    return srslte::get_if<State>(derived()->states);
  }

  std::string get_state_name() const
  {
    fsm_details::state_name_visitor visitor{};
    derived()->states.visit(visitor);
    return visitor.name;
  }

  //! Static method to check if State belongs to the list of possible states
  template <typename State>
  constexpr static bool can_hold_state()
  {
    return fsm_details::fsm_helper::get_fsm_state_list<fsm_t<Derived> >::template can_hold_type<State>();
  }

  void            set_fsm_event_log_level(srslte::LOG_LEVEL_ENUM e) { fsm_event_log_level = e; }
  srslte::log_ref get_log() const { return log_h; }

protected:
  friend struct fsm_details::fsm_helper;

  // Access to CRTP derived class
  derived_view*       derived() { return static_cast<derived_view*>(this); }
  const derived_view* derived() const { return static_cast<const derived_view*>(this); }

  void do_init()
  {
    fsm_details::fsm_helper::enter_visitor<derived_view> visitor{derived()};
    derived()->states.visit(visitor);
  }

  void enter() {}
  void exit() {}

  template <typename Event>
  void unhandled_event(Event&& e)
  {
    switch (fsm_event_log_level) {
      case LOG_LEVEL_DEBUG:
        log_h->debug("Unhandled event caught: \"%s\"\n", get_type_name<Event>().c_str());
        break;
      case LOG_LEVEL_INFO:
        log_h->info("Unhandled event caught: \"%s\"\n", get_type_name<Event>().c_str());
        break;
      case LOG_LEVEL_WARNING:
        log_h->warning("Unhandled event caught: \"%s\"\n", get_type_name<Event>().c_str());
        break;
      case LOG_LEVEL_ERROR:
        log_h->error("Unhandled event caught: \"%s\"\n", get_type_name<Event>().c_str());
        break;
      default:
        break;
    }
  }

  srslte::log_ref        log_h;
  srslte::LOG_LEVEL_ENUM fsm_event_log_level = LOG_LEVEL_DEBUG;
};

template <typename Derived, typename ParentFSM>
class nested_fsm_t : public fsm_t<Derived>
{
public:
  using base_t                = nested_fsm_t<Derived, ParentFSM>;
  using parent_t              = ParentFSM;
  static const bool is_nested = true;

  explicit nested_fsm_t(ParentFSM* parent_fsm_) : fsm_t<Derived>(parent_fsm_->get_log()), fsm_ptr(parent_fsm_) {}

  // Get pointer to outer FSM in case of HSM
  const parent_t* parent_fsm() const { return fsm_ptr; }
  parent_t*       parent_fsm() { return fsm_ptr; }

protected:
  using parent_fsm_t = ParentFSM;
  using fsm_t<Derived>::enter;
  using fsm_t<Derived>::do_init;

  ParentFSM* fsm_ptr = nullptr;
};

template <typename Proc>
struct proc_complete_ev {
  proc_complete_ev(bool success_) : success(success_) {}
  bool success;
};

// event
template <typename... Args>
struct proc_launch_ev {
  std::tuple<Args...> args;
  explicit proc_launch_ev(Args&&... args_) : args(std::forward<Args>(args_)...) {}
};

template <typename Derived, typename Result = srslte::same_state>
class proc_fsm_t : public fsm_t<Derived>
{
  using fsm_type = Derived;
  using fsm_t<Derived>::derived;
  friend struct fsm_details::fsm_helper;

protected:
  using fsm_t<Derived>::log_h;
  using fsm_t<Derived>::unhandled_event;
  void unhandled_event(srslte::proc_launch_ev<int*> e)
  {
    log_h->warning("Unhandled event \"launch\" caught when procedure is already running\n");
  }

public:
  using base_t = proc_fsm_t<Derived, Result>;
  using fsm_t<Derived>::trigger;

  // states
  struct idle_st {
  };
  struct complete_st {
    complete_st(bool success_) : success(success_) {}
    bool success;
  };

  explicit proc_fsm_t(srslte::log_ref log_) : fsm_t<Derived>(log_) {}

  bool is_running() const { return base_t::template is_in_state<idle_st>(); }

  template <typename... Args>
  void launch(Args&&... args)
  {
    trigger(proc_launch_ev<Args...>(std::forward<Args>(args)...));
  }

protected:
  void exit(idle_st& s)
  {
    launch_counter++;
    log_h->info("Starting run no. %d\n", launch_counter);
  }
  void enter(complete_st& s) { trigger(srslte::proc_complete_ev<bool>{s.success}); }

private:
  int launch_counter = 0;
};

} // namespace srslte

#endif // SRSLTE_FSM_H