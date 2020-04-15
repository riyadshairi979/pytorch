#include <c10/macros/Macros.h>

#include <ATen/core/op_registration/op_registration.h>
#if !defined(CAFFE2_IS_XPLAT_BUILD)
#include <torch/csrc/jit/frontend/function_schema_parser.h>
#endif

namespace c10 {

namespace {
  // TODO: Consider representing debug info as a struct instead so you
  // don't have to allocate strings all the time
  std::string debugString(std::string debug, const char* file, uint32_t line) {
#ifdef STRIP_ERROR_MESSAGES
    return "";
#else
    if (debug.empty()) {
      return c10::str("registered at ", file, ":", line);
    } else {
      return debug;
    }
#endif
  }

  std::ostream& operator<<(std::ostream& os, Library::Kind kind) {
    switch (kind) {
      case Library::DEF:
        os << "TORCH_LIBRARY";
        break;
      case Library::IMPL:
        os << "TORCH_LIBRARY_IMPL";
        break;
      case Library::FRAGMENT:
        os << "TORCH_LIBRARY_FRAGMENT";
        break;
    }
    return os;
  }
}

static_assert(std::is_nothrow_move_constructible<c10::optional<RegistrationHandleRAII>>::value, "");
static_assert(std::is_nothrow_move_assignable<c10::optional<RegistrationHandleRAII>>::value, "");

void RegisterOperators::checkSchemaAndRegisterOp_(Options&& options) {
  TORCH_CHECK(options.schemaOrName_.has_value(), "In operator registration: Tried to register an operator without specifying a schema or operator name.");
  if (options.schemaOrName_->is_right()) {
    // schema was explicitly specified. Check it matches the inferred one and register the op.

    const FunctionSchema& schema = options.schemaOrName_->right();

    checkNoDuplicateKernels_(options);

    registerOp_(std::move(options));
  } else {
    // schema wasn't explicitly specified. Take the inferred schema for registering the op.

    OperatorName name = std::move(*options.schemaOrName_).left();
    FunctionSchema inferred_schema = inferSchemaFromKernels_(name, options);

    options.schemaOrName_ = c10::make_right<OperatorName, FunctionSchema>(
      std::move(name.name),
      std::move(name.overload_name),
      inferred_schema.arguments(),
      inferred_schema.returns(),
      inferred_schema.is_vararg(),
      inferred_schema.is_varret()
    );

    checkNoDuplicateKernels_(options);

    // This would have unexpected behavior since an inferred schema will not
    // have aliasing annotations.
    TORCH_CHECK(
        options.aliasAnalysisKind_ != AliasAnalysisKind::FROM_SCHEMA,
        "In operator registration: Tried to register operator ",
        options.schemaOrName_->right(),
        " with AliasAnalysisKind::FROM_SCHEMA, but the schema is inferred.");

    // Register all kernels with the schema we inferred
    registerOp_(std::move(options));
  }
}

c10::FunctionSchema RegisterOperators::inferSchemaFromKernels_(const OperatorName& opName, const RegisterOperators::Options& options) {
  TORCH_CHECK(options.kernels.size() > 0, "Cannot infer operator schema in registration of operator ", opName, " because there is no kernel specified.");

  c10::optional<FunctionSchema> inferred_schema = c10::nullopt;
  for (const auto& kernel : options.kernels) {
    if (nullptr != kernel.inferred_function_schema.get()) {
      if (!inferred_schema.has_value()) {
        inferred_schema = *kernel.inferred_function_schema;
        break;
      }
    }
  }
  TORCH_CHECK(inferred_schema.has_value(), "Cannot infer operator schema for this kind of kernel in registration of operator ", opName, ". Please explicitly specify the operator schema or specify at least one kernel for which we can infer the schema.");

  return *inferred_schema;
}

void RegisterOperators::checkNoDuplicateKernels_(const Options& options) {
  std::unordered_set<DispatchKey> dispatch_keys;
  bool has_catchall_kernel = false;

  for (const auto& kernel : options.kernels) {
    if (kernel.dispatch_key.has_value()) {
      TORCH_CHECK(0 == dispatch_keys.count(*kernel.dispatch_key), "In operator registration: Tried to register multiple kernels with same dispatch key ", *kernel.dispatch_key, " for operator schema ", toString(options.schemaOrName_->right()));
      dispatch_keys.insert(*kernel.dispatch_key);
    } else {
      TORCH_CHECK(!has_catchall_kernel, "In operator registration: Tried to register multiple catch-all kernels for operator schema ", toString(options.schemaOrName_->right()));
      has_catchall_kernel = true;
    }
  }
}

void RegisterOperators::registerOp_(Options&& options) {
  FunctionSchema schema = std::move(*options.schemaOrName_).right();

  // HACK: bong in the alias analysis kind from the legacy API directly
  // into schema
  if (options.aliasAnalysisKind_.has_value()) {
    schema.setAliasAnalysis(*options.aliasAnalysisKind_);
  }

  OperatorName op_name = schema.operator_name();

  registrars_.emplace_back(
    Dispatcher::singleton().registerDef(std::move(schema), "registered by RegisterOperators")
  );

  for (auto& kernel : options.kernels) {
    registrars_.emplace_back(
      Dispatcher::singleton().registerImpl(op_name, kernel.dispatch_key, std::move(kernel.func), std::move(kernel.inferred_function_schema), "registered by RegisterOperators")
    );
  }
}

RegisterOperators::RegisterOperators() = default;
RegisterOperators::~RegisterOperators() = default;
RegisterOperators::RegisterOperators(RegisterOperators&&) noexcept = default;
RegisterOperators& RegisterOperators::operator=(RegisterOperators&&) noexcept = default;


CppFunction::CppFunction(KernelFunction func, std::unique_ptr<c10::FunctionSchema> schema)
  : func_(std::move(func))
  , schema_(std::move(schema))
  , debug_()
  {}

#define ERROR_CONTEXT "(Error occurred while processing ", kind_, " block at ", file_, ":", line_, ")"

Library::Library(Kind kind, std::string ns, c10::optional<DispatchKey> k, const char* file, uint32_t line)
  : kind_(kind)
  , ns_(ns == "_" ? c10::nullopt : c10::make_optional(std::move(ns)))
  , dispatch_key_((!k.has_value() || *k == DispatchKey::CatchAll) ? c10::nullopt : k)
  , file_(file)
  , line_(line)
  {
    switch (kind_) {
      case DEF:
        // Only DEFs require library uniqueness; fragments
        // don't register a library
        registrars_.emplace_back(
          Dispatcher::singleton().registerLibrary(
            *ns_, debugString("", file_, line_)
          )
        );
        // fallthrough
      case FRAGMENT:
        TORCH_CHECK(
          ns_.has_value(),
          kind_, ": cannot define ", kind_, " with the wildcard namespace _ "
          "(every ", kind_, " defines operators for a distinct namespace!)"
          "Did you mean to use TORCH_LIBRARY_IMPL instead?  "
          ERROR_CONTEXT
        );
        TORCH_INTERNAL_ASSERT(!dispatch_key_.has_value(), ERROR_CONTEXT);
        break;
      case IMPL:
        // Nothing to do, everything is OK
        break;
    }
  }

// TODO: Error if an operator is def'ed multiple times.  Right now we just
// merge everything

#define DEF_PRELUDE "def(\"", schema.operator_name(), "\"): "
Library& Library::_def(FunctionSchema&& schema, OperatorName* out_name) & {
  TORCH_CHECK(kind_ == DEF || kind_ == FRAGMENT,
    DEF_PRELUDE,
    "Cannot define an operator inside of a ", kind_, " block.  "
    "All def()s should be placed in the (unique) TORCH_LIBRARY block for their namespace.  ",
    ERROR_CONTEXT
  );
  TORCH_INTERNAL_ASSERT(ns_.has_value(), ERROR_CONTEXT);
  TORCH_INTERNAL_ASSERT(!dispatch_key_.has_value(), ERROR_CONTEXT);
  auto ns_opt = schema.getNamespace();
  if (ns_opt.has_value()) {
    // error case, but let's do a little more checking to see if
    // we can give a better message
    if (*ns_opt == *ns_) {
      TORCH_CHECK(false,
        DEF_PRELUDE,
        "Redundant definition of namespace (", *ns_, ") in both schema "
        "and the enclosing ", kind_, " block.  "
        "Delete the namespace from your schema string.  ",
        ERROR_CONTEXT
      );
    } else {
      TORCH_CHECK(false,
        DEF_PRELUDE,
        "Invalid explicit namespace (", *ns_opt, ") in schema string.  "
        "Move this definition to the (unique) TORCH_LIBRARY block for this namespace "
        "and delete the namespace from your schema string.  ",
        ERROR_CONTEXT
      );
    }
  } else {
    bool b = schema.setNamespaceIfNotSet(ns_->c_str());
    TORCH_INTERNAL_ASSERT(b, ERROR_CONTEXT);
  }
  if (out_name) {
    *out_name = schema.operator_name(); // copy!
  }
  registrars_.emplace_back(
    Dispatcher::singleton().registerDef(
      std::move(schema),
      debugString("", file_, line_)
    )
  );
  return *this;
}
#undef DEF_PRELUDE

Library& Library::_def(c10::either<OperatorName, FunctionSchema>&& name_or_schema, CppFunction&& f) & {
  FunctionSchema schema = [&] {
    if (name_or_schema.is_right()) {
      return std::move(name_or_schema).right();
    } else {
      // it's a name; use the inferred schema
      OperatorName name = std::move(name_or_schema).left();
      TORCH_CHECK(f.schema_,
        "def(\"", name, "\"): "
        "Full schema string was not specified, and we couldn't infer schema either.  ",
        "Please explicitly provide a schema string.  ",
        ERROR_CONTEXT
      );
      FunctionSchema s = f.schema_->cloneWithName(std::move(name.name), std::move(name.overload_name));
      s.setAliasAnalysis(c10::AliasAnalysisKind::CONSERVATIVE);
      return s;
    }
  }();
  OperatorName name("", "");  // Get the namespaced name for the impl call
  // First define the schema...
  _def(std::move(schema), &name);
  // Then register the implementation...
  auto dispatch_key = f.dispatch_key_.has_value() ? f.dispatch_key_ : dispatch_key_;
  registrars_.emplace_back(
    Dispatcher::singleton().registerImpl(
      std::move(name),
      dispatch_key,
      std::move(f.func_),
      std::move(f.schema_),
      debugString(std::move(f.debug_), file_, line_)
    )
  );
  return *this;
}

#define IMPL_PRELUDE "impl(\"", name_str, "\", ...): "
Library& Library::_impl(const char* name_str, CppFunction&& f) & {
  auto name = torch::jit::parseName(name_str);
  auto ns_opt = name.getNamespace();
  // This is kind of similar to the checking in def(), but the error
  // messages are a little different for this call site
  if (ns_opt.has_value()) {
    if (*ns_opt == *ns_) {
      TORCH_CHECK(false,
        IMPL_PRELUDE,
        "Redundant definition of namespace (", *ns_, ") in both operator name "
        "and the enclosing ", kind_, " block.  "
        "Delete the namespace from your operator name.  ",
        ERROR_CONTEXT
      );
    } else {
      TORCH_CHECK(false,
        IMPL_PRELUDE,
        "Invalid explicit namespace (", *ns_opt, ") in operator name.  "
        "Move this definition to ", kind_, " block for this namespace "
        "and delete the explicit namespace from your operator name.  ",
        ERROR_CONTEXT
      );
    }
  } else {
    bool b = name.setNamespaceIfNotSet(ns_->c_str());
    TORCH_INTERNAL_ASSERT(b, ERROR_CONTEXT);
  }
  TORCH_CHECK(!(f.dispatch_key_.has_value() && dispatch_key_.has_value()),
    IMPL_PRELUDE,
    "Explicitly provided dispatch key (", *f.dispatch_key_, ") is inconsistent "
    "with the dispatch key of the enclosing ", kind_, " block (", *dispatch_key_, ").  "
    "Please declare a separate ", kind_, " block for this dispatch key and "
    "move your impl() there.  "
    ERROR_CONTEXT
  );
  auto dispatch_key = f.dispatch_key_.has_value() ? f.dispatch_key_ : dispatch_key_;
  registrars_.emplace_back(
    Dispatcher::singleton().registerImpl(
      std::move(name),
      dispatch_key,
      std::move(f.func_),
      std::move(f.schema_),
      debugString(std::move(f.debug_), file_, line_)
    )
  );
  return *this;
}
#undef IMPL_PRELUDE

Library& Library::_fallback(CppFunction&& f) & {
  TORCH_CHECK(kind_ == IMPL,
    "fallback(...): Cannot define an operator inside of a ", kind_, " block.  "
    "Did you mean to call this function inside a TORCH_LIBRARY_IMPL block?  ",
    ERROR_CONTEXT);
  auto dispatch_key = f.dispatch_key_.has_value() ? f.dispatch_key_ : dispatch_key_;
  TORCH_INTERNAL_ASSERT(dispatch_key.has_value(), ERROR_CONTEXT);
  TORCH_CHECK(!ns_.has_value(),
    "fallback(...): Fallback functions which apply to only a single namespace ",
    "(you specified ", *ns_, ") are not supported.  If you intended to apply ",
    "this fallback function globally, please define a separate block:\n\n",
    "    TORCH_LIBRARY_IMPL(_, ", *dispatch_key, ", m) { m.fallback(...); }\n\n",
    ERROR_CONTEXT);
  registrars_.emplace_back(
    Dispatcher::singleton().registerFallback(
      *dispatch_key,
      std::move(f.func_),
      debugString(std::move(f.debug_), file_, line_)
    )
  );
  return *this;
}

}
