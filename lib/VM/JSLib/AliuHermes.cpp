#include "JSLibInternal.h"

#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/VM/HiddenClass.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <system_error>

namespace hermes {
namespace vm {

// AliuHermes.getBytecode(function)
CallResult<HermesValue>
hermesInternalGetBytecode(void *, Runtime *runtime, NativeArgs args) {
  auto func = args.dyncastArg<Callable>(0);
  if (!func) {
    return runtime->raiseTypeError(
        "Can't call HermesInternal.getBytecode() on non-callable");
  }

  /// Append the current function name to the \p strBuf.
  auto appendFunctionName = [&func, &runtime](SmallU16String<64> &strBuf) {
    // Extract the name.
    auto propRes = JSObject::getNamed_RJS(
        func, runtime, Predefined::getSymbolID(Predefined::name));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Convert the name to string, unless it is undefined.
    if (!(*propRes)->isUndefined()) {
      auto strRes =
          toString_RJS(runtime, runtime->makeHandle(std::move(*propRes)));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      strRes->get()->appendUTF16String(strBuf);
    }
    return ExecutionStatus::RETURNED;
  };

  SmallU16String<64> strBuf{};
  if (vmisa<JSAsyncFunction>(*func)) {
    strBuf.append("async function ");
  } else if (vmisa<JSGeneratorFunction>(*func)) {
    strBuf.append("function *");
  } else {
    strBuf.append("function ");
  }

  if (LLVM_UNLIKELY(appendFunctionName(strBuf) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Formal parameters and the rest of the body.
  if (vmisa<NativeFunction>(*func)) {
    // Use [native code] here because we want to work with tools like Babel
    // which detect the string "[native code]" and use it to alter behavior
    // during the class transform.
    // Also print without synthesized formal parameters to avoid breaking
    // heuristics that detect the string "() { [native code] }".
    // \see https://github.com/facebook/hermes/issues/471
    strBuf.append("() { [native code] }");
  } else {
    // Append the synthesized formal parameters.
    strBuf.append('(');

    // Extract ".length".
    auto lengthProp = Callable::extractOwnLengthProperty_RJS(func, runtime);
    if (lengthProp == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;

    // The value of the property is not guaranteed to be meaningful, so clamp it
    // to [0..65535] for sanity.
    uint32_t paramCount =
        (uint32_t)std::min(65535.0, std::max(0.0, *lengthProp));

    for (uint32_t i = 0; i < paramCount; ++i) {
      if (i != 0)
        strBuf.append(", ");
      char buf[16];
      ::snprintf(buf, sizeof(buf), "a%u", i);
      strBuf.append(buf);
    }

    strBuf.append(") {\n");

    if (auto jsFunc = dyn_vmcast<JSFunction>(*func)) {
      auto funcId = jsFunc->getCodeBlock()->getFunctionID();

      hbc::BytecodeDisassembler disassembler(
          jsFunc->getRuntimeModule()->getBytecodeSharedPtr());
      hbc::DisassemblyOptions options = hbc::DisassemblyOptions::IncludeSource |
          hbc::DisassemblyOptions::IncludeFunctionIds |
          hbc::DisassemblyOptions::Pretty;
      disassembler.setOptions(options);

      std::string str;
      llvh::raw_string_ostream output(str);
      disassembler.disassembleFunction(funcId, output);

      strBuf.append(output.str());
    } else {
      // Avoid using the [native code] string to prevent extra wrapping overhead
      // in, e.g., Babel's class extension mechanism.
      strBuf.append("    [bytecode]\n");
    }

    strBuf.append("}");
  }

  // Finally allocate a StringPrimitive.
  return StringPrimitive::create(runtime, strBuf);
}

class MappedFileBuffer : public Buffer {
 public:
  char *error = nullptr;

  explicit MappedFileBuffer(const std::string &fileName) {
    fd_ = open(fileName.c_str(), O_RDONLY);
    if (fd_ < 0) {
      error = strerror(errno);
      return;
    }
    struct stat statbuf;
    if (fstat(fd_, &statbuf) < 0) {
      error = strerror(errno);
      return;
    }
    size_ = statbuf.st_size;
    void *bytecodeFileMap = mmap(
        /*address*/ nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, /*offset*/ 0);
    if (bytecodeFileMap == MAP_FAILED) {
      error = "mmaped failed";
      return;
    }
    data_ = reinterpret_cast<uint8_t *>(bytecodeFileMap);
  }

  ~MappedFileBuffer() override {
    if (munmap((void *)data_, size_) < 0) {
      assert(false && "Failed to munmap MappedFileBuffer");
    }
    if (close(fd_) < 0) {
      assert(false && "Failed to close MappedFileBuffer");
    }
  }

 private:
  int fd_;
};

// AliuHermes.run(path: string)
CallResult<HermesValue>
hermesInternalRun(void *, Runtime *runtime, NativeArgs args) {
  if (!args.getArg(0).isString()) {
    return runtime->raiseTypeError("Path has to be a string");
  }

  auto str = args.dyncastArg<StringPrimitive>(0);
  std::string path;
  auto view = StringPrimitive::createStringView(runtime, str);
  if (view.isASCII()) {
    path = std::string(view.begin(), view.end());
  } else {
    SmallU16String<4> allocator;
    convertUTF16ToUTF8WithReplacements(path, view.getUTF16Ref(allocator));
  }

  auto buffer = std::make_unique<MappedFileBuffer>(path);
  if (buffer->error) {
    return runtime->raiseError(buffer->error);
  }

  auto bytecode_err =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer));
  if (!bytecode_err.first) {
    return runtime->raiseSyntaxError(TwineChar16(bytecode_err.second));
  }

  auto bytecode = std::move(bytecode_err.first);

  return runtime->runBytecode(
      std::move(bytecode),
      RuntimeModuleFlags{},
      path,
      Runtime::makeNullHandle<Environment>(),
      runtime->getGlobal());
}

using namespace hermes::hbc;
using namespace hermes::inst;

class StringVisitor : public BytecodeVisitor {
 private:
  inst::OpCode opcode_;
  uint32_t i_ = 0;

 protected:
  void preVisitInstruction(OpCode opcode, const uint8_t *ip, int length)
      override {
    opcode_ = opcode;
  }

  CallResult<HermesValue> makeStr(
      ArrayRef<unsigned char> storage,
      StringTableEntry entry) {
    if (entry.isUTF16()) {
      const char16_t *s =
          (const char16_t *)(storage.begin() + entry.getOffset());
      return StringPrimitive::create(runtime_, UTF16Ref{s, entry.getLength()});
    } else {
      const char *s = (const char *)storage.begin() + entry.getOffset();
      return StringPrimitive::create(runtime_, ASCIIRef{s, entry.getLength()});
    }
  }

  void visitString(StringID stringID) {
    auto storage = bcProvider_->getStringStorage();
    auto entry = bcProvider_->getStringTableEntry(stringID);

    CallResult<HermesValue> strResult = makeStr(storage, entry);

    auto str = runtime_->makeHandle<StringPrimitive>(*strResult);

    JSArray::setElementAt(array_, runtime_, i_, str);
    i_++;
  }

  void visitOperand(
      const uint8_t *ip,
      OperandType operandType,
      const uint8_t *operandBuf,
      int operandIndex) override {
    const bool isStringID = isOperandStringID(opcode_, operandIndex);
    if (!isStringID)
      return;

    switch (operandType) {
#define DEFINE_OPERAND_TYPE(name, ctype)         \
  case OperandType::name: {                      \
    ctype operandVal;                            \
    decodeOperand(operandBuf, &operandVal);      \
    if (operandType == OperandType::Addr8 ||     \
        operandType == OperandType::Addr32) {    \
      /* operandVal is relative to current ip.*/ \
      return;                                    \
    }                                            \
    visitString(operandVal);                     \
    break;                                       \
  }
#include "hermes/BCGen/HBC/BytecodeList.def"
    }
  }

  void afterStart() override {
    JSArray::setLengthProperty(array_, runtime_, i_);
  }

 public:
  hermes::vm::Handle<hermes::vm::JSArray> array_;
  Runtime *runtime_;
  StringVisitor(
      std::shared_ptr<hbc::BCProvider> bcProvider,
      hermes::vm::Handle<hermes::vm::JSArray> array,
      Runtime *runtime)
      : BytecodeVisitor(bcProvider), array_(array), runtime_(runtime) {}
};

// AliuHermes.findStrings(function): string[]
CallResult<HermesValue>
hermesInternalFindStrings(void *, Runtime *runtime, NativeArgs args) {
  auto func = args.dyncastArg<JSFunction>(0);
  if (!func) {
    return runtime->raiseTypeError(
        "Can't call HermesInternal.findStrings() on non-function");
  }

  auto funcId = func->getCodeBlock()->getFunctionID();

  auto arrayResult = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrayResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto array = *arrayResult;

  StringVisitor visitor(
      func->getRuntimeModule()->getBytecodeSharedPtr(), array, runtime);
  visitor.visitInstructionsInFunction(funcId);

  return visitor.array_.getHermesValue();
}

void allowExtensions(Handle<JSObject> selfHandle, Runtime *runtime) {
  if (LLVM_UNLIKELY(selfHandle->isProxyObject())) {
    auto target = runtime->makeHandle(detail::slots(*selfHandle).target);
    allowExtensions(target, runtime);
    return;
  }

  selfHandle->flags_.noExtend = false;
}

// reversed HiddenClass::makeAllReadOnly
Handle<HiddenClass> makeAllWriteable(
    Handle<HiddenClass> selfHandle,
    Runtime *runtime) {
  if (!selfHandle->propertyMap_)
    HiddenClass::initializeMissingPropertyMap(selfHandle, runtime);

  auto mapHandle = runtime->makeHandle(selfHandle->propertyMap_);

  MutableHandle<HiddenClass> curHandle{runtime, *selfHandle};

  DictPropertyMap::forEachProperty(
      mapHandle,
      runtime,
      [runtime, &curHandle](SymbolID id, NamedPropertyDescriptor desc) {
        PropertyFlags newFlags = desc.flags;
        if (!newFlags.accessor) {
          newFlags.writable = 1;
          newFlags.configurable = 1;
        } else {
          newFlags.configurable = 1;
        }
        if (desc.flags == newFlags)
          return;

        assert(
            curHandle->propertyMap_ &&
            "propertyMap must exist after updateOwnProperty()");

        auto found =
            DictPropertyMap::find(curHandle->propertyMap_.get(runtime), id);
        assert(found && "property not found during enumeration");
        curHandle = *HiddenClass::updateProperty(curHandle, runtime, *found, newFlags);
      });

  curHandle->flags_.allNonConfigurable = false;
  curHandle->flags_.allReadOnly = false;

  return std::move(curHandle);
}

// AliuHermes.unfreeze<T>(T): T
CallResult<HermesValue>
hermesInternalUnfreeze(void *, Runtime *runtime, NativeArgs args) {
  auto objHandle = args.dyncastArg<JSObject>(0);
  if (!objHandle) {
    return args.getArg(0);
  }

  allowExtensions(objHandle, runtime);

  auto newClazz = makeAllWriteable(runtime->makeHandle(objHandle->clazz_), runtime);
  objHandle->clazz_.setNonNull(runtime, *newClazz, &runtime->getHeap());

  objHandle->flags_.frozen = false;
  objHandle->flags_.sealed = false;

  return objHandle.getHermesValue();
}

Handle<JSObject> createAliuHermesObject(
    Runtime *runtime,
    const JSLibFlags &flags) {
  namespace P = Predefined;
  Handle<JSObject> intern = runtime->makeHandle(JSObject::create(runtime));
  GCScope gcScope{runtime};

  DefinePropertyFlags constantDPF =
      DefinePropertyFlags::getDefaultNewPropertyFlags();
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  auto defineInternMethod =
      [&](Predefined::Str symID, NativeFunctionPtr func, uint8_t count = 0) {
        (void)defineMethod(
            runtime,
            intern,
            Predefined::getSymbolID(symID),
            nullptr /* context */,
            func,
            count,
            constantDPF);
      };

  defineInternMethod(P::getBytecode, hermesInternalGetBytecode);
  defineInternMethod(P::run, hermesInternalRun);
  defineInternMethod(P::findStrings, hermesInternalFindStrings);
  defineInternMethod(P::unfreeze, hermesInternalUnfreeze);

  JSObject::preventExtensions(*intern);

  return intern;
}

} // namespace vm
} // namespace hermes
