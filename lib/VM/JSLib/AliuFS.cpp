#include "JSLibInternal.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <random>
#include "hermes/VM/JSArrayBuffer.h"
#include <cstdio>
#include <cerrno>

namespace hermes {
namespace vm {

std::string toString(
    Runtime *runtime,
    hermes::vm::Handle<hermes::vm::StringPrimitive> handle) {
  auto view = StringPrimitive::createStringView(runtime, handle);
  if (view.isASCII()) {
    return std::string(view.begin(), view.end());
  } else {
    SmallU16String<4> allocator;
    std::string result;
    convertUTF16ToUTF8WithReplacements(result, view.getUTF16Ref(allocator));
    return result;
  }
}

// AliuFS.mkdir(path: string)
CallResult<HermesValue> aliuFSmkdir(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path must be a string");
  }

  auto path = toString(runtime, pathHandle);

  ::hermes::hermesLog("AliuHermes", "AliuFS.mkdir %s", path.c_str());

  if (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
    if (errno != EEXIST)
      return runtime->raiseError(static_cast<const StringRef>(
          std::string(strerror(errno)) + ": " + path));
  }

  return HermesValue::encodeUndefinedValue();
}

// AliuFS.exists(path: string): boolean
CallResult<HermesValue>
aliuFSexists(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path must be a string");
  }

  auto path = toString(runtime, pathHandle);

  auto exists = access(path.c_str(), F_OK) == 0;

  ::hermes::hermesLog(
      "AliuHermes", "AliuFS.exists %s = %d", path.c_str(), exists);

  return runtime->getBoolValue(exists).getHermesValue();
}

// AliuFS.readdir(path: string): { name: string, type: "file" | "directory" }[]
CallResult<HermesValue>
aliuFSreaddir(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path must be a string");
  }

  auto path = toString(runtime, pathHandle);

  ::hermes::hermesLog("AliuHermes", "AliuFS.readdir %s", path.c_str());

  auto dir = opendir(path.c_str());
  if (!dir) {
    return runtime->raiseError(strerror(errno));
  }

  auto arrayResult = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrayResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto array = *arrayResult;

  dirent *entry;
  auto i = 0;
  while ((entry = readdir(dir)) != nullptr) {
    auto nameResult =
        StringPrimitive::create(runtime, createASCIIRef(entry->d_name));
    if (LLVM_UNLIKELY(nameResult == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto name = runtime->makeHandle<StringPrimitive>(*nameResult);

    Handle<JSObject> entryHandle =
        runtime->makeHandle(JSObject::create(runtime));
    defineProperty(
        runtime, entryHandle, Predefined::getSymbolID(Predefined::name), name);

    defineProperty(
        runtime,
        entryHandle,
        Predefined::getSymbolID(Predefined::type),
        runtime->getPredefinedStringHandle(
            entry->d_type == DT_DIR ? Predefined::directory
                                    : Predefined::file));

    JSArray::setElementAt(array, runtime, i, entryHandle);
    i++;
  }
  closedir(dir);
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(array, runtime, i) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return array.getHermesValue();
}

// AliuFS.writeFile(path: string, content: string | ArrayBuffer)
CallResult<HermesValue>
aliuFSwriteFile(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path must be a string");
  }

  auto path = toString(runtime, pathHandle);

  ::hermes::hermesLog("AliuHermes", "AliuFS.write %s", path.c_str());

  if (auto textHandle = args.dyncastArg<StringPrimitive>(1)) {
    auto text = toString(runtime, textHandle);

    auto f = fopen(path.c_str(), "w");
    if (!f) {
      return runtime->raiseError(strerror(errno));
    }

    if (fputs(text.c_str(), f) < 0) {
      fclose(f);
      return runtime->raiseError(strerror(errno));
    }

    fclose(f);

    return HermesValue::encodeUndefinedValue();
  }

  if (auto buffer = args.dyncastArg<JSArrayBuffer>(1)) {
    auto dataBlock = buffer->getDataBlock();
    auto size = buffer->size();

    auto f = fopen(path.c_str(), "wb");
    if (!f) {
      return runtime->raiseError(strerror(errno));
    }

    if (fwrite(dataBlock, sizeof(uint8_t), size, f) != size) {
      fclose(f);
      return runtime->raiseError(strerror(errno));
    }

    fclose(f);

    return HermesValue::encodeUndefinedValue();
  }

  return runtime->raiseTypeError(
      "Content must be a string or ArrayBuffer");
}

// AliuFS.readFile(path: string, encoding: "text" | "binary" = "text"): string |
// ArrayBuffer
CallResult<HermesValue>
aliuFSreadFile(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path must be a string");
  }

  auto path = toString(runtime, pathHandle);

  ::hermes::hermesLog("AliuHermes", "AliuFS.read %s", path.c_str());

  auto encodingHandle = args.dyncastArg<StringPrimitive>(1);
  if (!encodingHandle) {
    return runtime->raiseTypeError("Encoding must be a string");
  }

  auto encoding = toString(runtime, encodingHandle);

  if (encoding == "text") {
    auto f = fopen(path.c_str(), "rb");
    if (!f) {
      return runtime->raiseError(strerror(errno));
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char s[size];

    if (fread(s, 1, size, f) != size) {
      fclose(f);
      return runtime->raiseError(strerror(errno));
    }

    fclose(f);

    return runtime
        ->makeHandle<StringPrimitive>(
            *StringPrimitive::create(runtime, ASCIIRef(s, size)))
        .getHermesValue();
  }

  if (encoding == "binary") {
    auto f = fopen(path.c_str(), "rb");
    if (!f) {
      return runtime->raiseError(strerror(errno));
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    auto buffer = runtime->makeHandle(JSArrayBuffer::create(
        runtime, Handle<JSObject>::vmcast(&runtime->arrayBufferPrototype)));

    if (LLVM_UNLIKELY(
            buffer->createDataBlock(runtime, size, false) ==
            ExecutionStatus::EXCEPTION)) {
      fclose(f);
      return ExecutionStatus::EXCEPTION;
    }

    if (fread(buffer->getDataBlock(), sizeof(uint8_t), size, f) != size) {
      fclose(f);
      buffer->detach(&runtime->getHeap());
      return runtime->raiseError(strerror(errno));
    }

    fclose(f);

    return buffer.getHermesValue();
  }

  return runtime->raiseTypeError(R"(Encoding must be "text" or "binary")");
}

// AliuFS.remove(path: string, opts?: Record<"force" | "recursive", boolean>): void
CallResult<HermesValue> aliuFSremove(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("path must be a string");
  }
  auto path = toString(runtime, pathHandle);

  bool force = false;
  bool recursive = false;

  if (auto optsHandle = args.dyncastArg<JSObject>(1)) {
#define GET_PROP(variable, prop, name) do { \
    auto res = JSObject::getNamed_RJS(optsHandle, runtime, prop); \
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) \
      return ExecutionStatus::EXCEPTION; \
    if ((*res)->isUndefined()) break; \
    if (LLVM_UNLIKELY(!(*res)->isBool())) \
      return runtime->raiseTypeError(static_cast<const StringRef>(std::string(name) + " must be a boolean")); \
    variable = (*res)->getBool(); \
    } while(false)

    GET_PROP(force, Predefined::getSymbolID(Predefined::force), "force");
    GET_PROP(recursive, Predefined::getSymbolID(Predefined::recursive), "recursive");
#undef GET_PROP

    if (recursive) {
      return runtime->raiseError("Oops recursive not implemented yet :troll:");
    }
  } else if (!args.getArg(1).isUndefined()) {
    return runtime->raiseTypeError("options must be an object");
  }

  int res = std::remove(path.c_str());
  if (res == 0 || (errno == ENOENT && force)) {
    return HermesValue::encodeUndefinedValue();
  }

  return runtime->raiseError(static_cast<const StringRef>(
      std::string(strerror(errno)) + ": " + path));
}

Handle<JSObject> createAliuFSObject(Runtime *runtime, const JSLibFlags &flags) {
  namespace P = Predefined;
  Handle<JSObject> intern = runtime->makeHandle(JSObject::create(runtime));
  GCScope gcScope{runtime};

  DefinePropertyFlags constantDPF =
      DefinePropertyFlags::getDefaultNewPropertyFlags();
  constantDPF.enumerable = 1;
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

  defineInternMethod(P::mkdir, aliuFSmkdir);
  defineInternMethod(P::readdir, aliuFSreaddir);
  defineInternMethod(P::exists, aliuFSexists);
  defineInternMethod(P::writeFile, aliuFSwriteFile);
  defineInternMethod(P::readFile, aliuFSreadFile);
  defineInternMethod(P::remove, aliuFSremove);


  JSObject::preventExtensions(*intern);

  return intern;
}

} // namespace vm
} // namespace hermes
