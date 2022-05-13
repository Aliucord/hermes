#include "JSLibInternal.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <random>
#include "hermes/VM/JSArrayBuffer.h"

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
    return runtime->raiseTypeError("Path has to be a string");
  }

  auto path = toString(runtime, pathHandle);

  ::hermes::hermesLog("AliuHermes", "AliuFS.mkdir %s", path.c_str());

  if (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) {
    if (errno != EEXIST)
      return runtime->raiseError(strerror(errno));
  }

  return HermesValue::encodeUndefinedValue();
}

// AliuFS.exists(path: string): boolean
CallResult<HermesValue>
aliuFSexists(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path has to be a string");
  }

  auto path = toString(runtime, pathHandle);

  struct stat buffer;
  auto exists = stat(path.c_str(), &buffer) == 0;

  ::hermes::hermesLog(
      "AliuHermes", "AliuFS.exists %s = %d", path.c_str(), exists);

  return runtime->getBoolValue(exists).getHermesValue();
}

// AliuFS.readdir(path: string): { name: string, type: "file" | "directory" }[]
CallResult<HermesValue>
aliuFSreaddir(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path has to be a string");
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
    return runtime->raiseTypeError("Path has to be a string");
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
      "Content has to be a string or an ArrayBuffer");
}

// AliuFS.readFile(path: string, encoding: "text" | "binary" = "text"): string |
// ArrayBuffer
CallResult<HermesValue>
aliuFSreadFile(void *, Runtime *runtime, NativeArgs args) {
  auto pathHandle = args.dyncastArg<StringPrimitive>(0);
  if (!pathHandle) {
    return runtime->raiseTypeError("Path has to be a string");
  }

  auto path = toString(runtime, pathHandle);

  ::hermes::hermesLog("AliuHermes", "AliuFS.read %s", path.c_str());

  auto encodingHandle = args.dyncastArg<StringPrimitive>(1);
  if (!encodingHandle) {
    return runtime->raiseTypeError("Encoding has to be a string");
  }

  auto encoding = toString(runtime, encodingHandle);

  if (encoding == "text") {
    auto f = fopen(path.c_str(), "rb");
    if (!f) {
      return runtime->raiseError(strerror(errno));
    }

    fseek(f, 0, SEEK_END);
    auto size = ftell(f);
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
    auto size = ftell(f);
    fseek(f, 0, SEEK_SET);

    auto buffer = runtime->makeHandle(JSArrayBuffer::create(
        runtime, Handle<JSObject>::vmcast(&runtime->arrayBufferPrototype)));

    if (LLVM_UNLIKELY(
            buffer->createDataBlock(runtime, size, false) ==
            ExecutionStatus::EXCEPTION)) {
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

  return runtime->raiseTypeError("Encoding has to be \"text\" or \"binary\"");
}

Handle<JSObject> createAliuFSObject(Runtime *runtime, const JSLibFlags &flags) {
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

  defineInternMethod(P::mkdir, aliuFSmkdir);
  defineInternMethod(P::readdir, aliuFSreaddir);
  defineInternMethod(P::exists, aliuFSexists);
  defineInternMethod(P::writeFile, aliuFSwriteFile);
  defineInternMethod(P::readFile, aliuFSreadFile);

  JSObject::preventExtensions(*intern);

  return intern;
}

} // namespace vm
} // namespace hermes
