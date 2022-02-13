NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

template <bool ReturnRef, typename Func, typename Return, typename... Args,
          size_t... Is, typename... Extra>
NB_INLINE PyObject *func_create(Func &&f, Return (*)(Args...),
                                std::index_sequence<Is...>,
                                const Extra &...extra) {
    // Detect locations of nb::args / nb::kwargs (if exists)
    static constexpr size_t
        args_pos_1 = index_1_v<std::is_same_v<intrinsic_t<Args>, args>...>,
        args_pos_n = index_n_v<std::is_same_v<intrinsic_t<Args>, args>...>,
        kwargs_pos_1 = index_1_v<std::is_same_v<intrinsic_t<Args>, kwargs>...>,
        kwargs_pos_n = index_n_v<std::is_same_v<intrinsic_t<Args>, kwargs>...>,
        nargs = sizeof...(Args);

    // Determine the number of nb::arg/nb::arg_v annotations
    constexpr size_t nargs_provided =
        ((std::is_same_v<arg, Extra> + std::is_same_v<arg_v, Extra>) + ...);

    /// A few compile-time consistency checks
    static_assert(args_pos_1 == args_pos_n && kwargs_pos_1 == kwargs_pos_n,
        "Repeated use of nb::kwargs or nb::args in the function signature!");
    static_assert(nargs_provided == 0 || nargs_provided == nargs,
        "The number of nb::arg annotations must match the argument count!");
    static_assert(kwargs_pos_1 == nargs || kwargs_pos_1 + 1 == nargs,
        "nb::kwargs must be the last element of the function signature!");
    static_assert(args_pos_1 == nargs || args_pos_1 + 1 == kwargs_pos_1,
        "nb::args must follow positional arguments and precede nb::kwargs!");

    // Collect function signature information for the docstring
    constexpr bool is_void_ret = std::is_void_v<Return>;
    using cast_out =
        make_caster<std::conditional_t<is_void_ret, std::nullptr_t, Return>>;
    constexpr auto descr =
        const_name("(") + concat(type_descr(make_caster<Args>::cname)...) +
        const_name(") -> ") + cast_out::cname;
    const std::type_info* descr_types[descr.type_count() + 1];
    descr.put_types(descr_types);

    // Auxiliary data structure to capture the provided function/closure
    struct capture {
        std::remove_reference_t<Func> f;
    };

    // The following temporary record will describe the function in detail
    func_data<nargs_provided> data;
    data.flags = (args_pos_1   < nargs ? (uint16_t) func_flags::has_var_args     : 0) |
                 (kwargs_pos_1 < nargs ? (uint16_t) func_flags::has_var_kwargs   : 0) |
                 (nargs_provided       ? (uint16_t) func_flags::has_args : 0) |
                 (ReturnRef            ? (uint16_t) func_flags::return_ref   : 0);

    // Store captured function inside 'func_data' if there is space. Issues
    // with aliasing are resolved via separate compilation of libnanobind
    if constexpr (sizeof(capture) <= sizeof(data.capture)) {
        capture *cap = (capture *) data.capture;
        new (cap) capture{ (forward_t<Func>) f };

        if constexpr (!std::is_trivially_destructible_v<capture>) {
            data.flags |= (uint16_t) func_flags::has_free;
            data.free = [](void *p) {
                ((capture *) p)->~capture();
            };
        }
    } else {
        void **cap = (void **) data.capture;
        cap[0] = new capture{ (forward_t<Func>) f };

        data.flags |= (uint16_t) func_flags::has_free;
        data.free = [](void *p) {
            delete (capture *) ((void **) p)[0];
        };
    }

    data.impl = [](void *p, PyObject **args, bool *args_convert,
                   PyObject *parent) -> PyObject * {
        const capture *cap;
        if constexpr (sizeof(capture) <= sizeof(data.capture))
            cap = (capture *) p;
        else
            cap = (capture *) ((void **) p)[0];

        nb_tuple<make_caster<Args>...> in;
        if ((!in.template get<Is>().load(args[Is], args_convert[Is]) || ...))
            return NB_NEXT_OVERLOAD;

        if constexpr (is_void_ret) {
            cap->f(
                in.template get<Is>().operator typename make_caster<Args>::
                    template cast_op_type<Args>()...),
            Py_INCREF(Py_None);
            return Py_None;
        } else {
            rv_policy policy = rv_policy::automatic;
            return cast_out::cast(
                cap->f(
                    in.template get<Is>().operator typename make_caster<Args>::
                        template cast_op_type<Args>()...),
                policy, parent).ptr();
        }
    };

    data.descr = descr.text;
    data.descr_types = descr_types;
    data.nargs = (uint16_t) nargs;

    // Fill remaining fields of 'data'
    size_t arg_index = 0;
    (detail::func_extra_apply(data, extra, arg_index), ...);

    return func_new((void *) &data);
}

NAMESPACE_END(detail)

template <typename Return, typename... Args, typename... Extra>
NB_INLINE object cpp_function(Return (*f)(Args...), const Extra&... extra) {
    return steal(detail::func_create<true>(
        f, f, std::make_index_sequence<sizeof...(Args)>(), extra...));
}

template <typename Return, typename... Args, typename... Extra>
NB_INLINE void cpp_function_def(Return (*f)(Args...), const Extra&... extra) {
    detail::func_create<false>(
        f, f, std::make_index_sequence<sizeof...(Args)>(), extra...);
}

/// Construct a cpp_function from a lambda function (pot. with internal state)
template <
    typename Func, typename... Extra,
    detail::enable_if_t<detail::is_lambda_v<std::remove_reference_t<Func>>> = 0>
NB_INLINE object cpp_function(Func &&f, const Extra &...extra) {
    using am = detail::analyze_method<decltype(&Func::operator())>;
    return steal(detail::func_create<true>(
        (detail::forward_t<Func>) f, (typename am::func *) nullptr,
        std::make_index_sequence<am::argc>(), extra...));
}

template <
    typename Func, typename... Extra,
    detail::enable_if_t<detail::is_lambda_v<std::remove_reference_t<Func>>> = 0>
NB_INLINE void cpp_function_def(Func &&f, const Extra &...extra) {
    using am = detail::analyze_method<decltype(&Func::operator())>;
    detail::func_create<false>(
        (detail::forward_t<Func>) f, (typename am::func *) nullptr,
        std::make_index_sequence<am::argc>(), extra...);
}

/// Construct a cpp_function from a class method (non-const)
template <typename Return, typename Class, typename... Args, typename... Extra>
NB_INLINE object cpp_function(Return (Class::*f)(Args...), const Extra &...extra) {
    return steal(detail::func_create<true>(
        [f](Class *c, Args... args) NB_INLINE_LAMBDA -> Return {
            return (c->*f)((detail::forward_t<Args>) args...);
        },
        (Return(*)(Class *, Args...)) nullptr,
        std::make_index_sequence<sizeof...(Args) + 1>(), extra...));
}

template <typename Return, typename Class, typename... Args, typename... Extra>
NB_INLINE void cpp_function_def(Return (Class::*f)(Args...), const Extra &...extra) {
    detail::func_create<false>(
        [f](Class *c, Args... args) NB_INLINE_LAMBDA -> Return {
            return (c->*f)((detail::forward_t<Args>) args...);
        },
        (Return(*)(Class *, Args...)) nullptr,
        std::make_index_sequence<sizeof...(Args) + 1>(), extra...);
}

/// Construct a cpp_function from a class method (const)
template <typename Return, typename Class, typename... Args, typename... Extra>
NB_INLINE object cpp_function(Return (Class::*f)(Args...) const, const Extra &...extra) {
    return steal(detail::func_create<true>(
        [f](const Class *c, Args... args) NB_INLINE_LAMBDA -> Return {
            return (c->*f)((detail::forward_t<Args>) args...);
        },
        (Return(*)(const Class *, Args...)) nullptr,
        std::make_index_sequence<sizeof...(Args) + 1>(), extra...));
}

template <typename Return, typename Class, typename... Args, typename... Extra>
NB_INLINE void cpp_function_def(Return (Class::*f)(Args...) const, const Extra &...extra) {
    detail::func_create<false>(
        [f](const Class *c, Args... args) NB_INLINE_LAMBDA -> Return {
            return (c->*f)((detail::forward_t<Args>) args...);
        },
        (Return(*)(const Class *, Args...)) nullptr,
        std::make_index_sequence<sizeof...(Args) + 1>(), extra...);
}

template <typename Func, typename... Extra>
module_ &module_::def(const char *name_, Func &&f,
                                const Extra &...extra) {
    cpp_function_def((detail::forward_t<Func>) f, scope(*this),
                     name(name_), extra...);
    return *this;
}

NAMESPACE_END(NB_NAMESPACE)
