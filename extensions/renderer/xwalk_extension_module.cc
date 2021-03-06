// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright (c) 2013 Intel Corporation. All rights reserved.
// Copyright (c) 2015 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/renderer/xwalk_extension_module.h"

#include <v8/v8.h>
#include <stdarg.h>
#include <stdio.h>

#include <vector>

#include "common/logger.h"
#include "extensions/renderer/runtime_ipc_client.h"
#include "extensions/renderer/xwalk_extension_client.h"
#include "extensions/renderer/xwalk_module_system.h"

// The arraysize(arr) macro returns the # of elements in an array arr.
// The expression is a compile-time constant, and therefore can be
// used in defining new arrays, for example.  If you use arraysize on
// a pointer by mistake, you will get a compile-time error.
//
// One caveat is that arraysize() doesn't accept any array of an
// anonymous type or a type defined inside a function.  In these rare
// cases, you have to use the unsafe ARRAYSIZE_UNSAFE() macro below.  This is
// due to a limitation in C++'s template system.  The limitation might
// eventually be removed, but it hasn't happened yet.

// This template function declaration is used in defining arraysize.
// Note that the function doesn't need an implementation, as we only
// use its type.
template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];

#define arraysize(array) (sizeof(ArraySizeHelper(array)))

namespace extensions {

namespace {

// This is the key used in the data object passed to our callbacks to store a
// pointer back to kXWalkExtensionModule.
const char* kXWalkExtensionModule = "kXWalkExtensionModule";

}  // namespace

XWalkExtensionModule::XWalkExtensionModule(XWalkExtensionClient* client,
                                           XWalkModuleSystem* module_system,
                                           const std::string& extension_name) :
    extension_name_(extension_name),
    client_(client),
    module_system_(module_system) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::Object> function_data = v8::Object::New(isolate);
  function_data->Set(v8::String::NewFromUtf8(isolate, kXWalkExtensionModule),
                     v8::External::New(isolate, this));

  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  // TODO(cmarcelo): Use Template::Set() function that takes isolate, once we
  // update the Chromium (and V8) version.
  object_template->Set(
      v8::String::NewFromUtf8(isolate, "postMessage"),
      v8::FunctionTemplate::New(isolate, PostMessageCallback, function_data));
  object_template->Set(
      v8::String::NewFromUtf8(isolate, "sendSyncMessage"),
      v8::FunctionTemplate::New(
          isolate, SendSyncMessageCallback, function_data));
  object_template->Set(
      v8::String::NewFromUtf8(isolate, "setMessageListener"),
      v8::FunctionTemplate::New(
          isolate, SetMessageListenerCallback, function_data));
  object_template->Set(
      v8::String::NewFromUtf8(isolate, "sendRuntimeMessage"),
      v8::FunctionTemplate::New(
          isolate, SendRuntimeMessageCallback, function_data));
  object_template->Set(
      v8::String::NewFromUtf8(isolate, "sendRuntimeSyncMessage"),
      v8::FunctionTemplate::New(
          isolate, SendRuntimeSyncMessageCallback, function_data));
  object_template->Set(
      v8::String::NewFromUtf8(isolate, "sendRuntimeAsyncMessage"),
      v8::FunctionTemplate::New(
          isolate, SendRuntimeAsyncMessageCallback, function_data));

  function_data_.Reset(isolate, function_data);
  object_template_.Reset(isolate, object_template);
}

XWalkExtensionModule::~XWalkExtensionModule() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);

  // Deleting the data will disable the functions, they'll return early. We do
  // this because it might be the case that the JS objects we created outlive
  // this object (getting references from inside an iframe and then destroying
  // the iframe), even if we destroy the references we have.
  v8::Handle<v8::Object> function_data =
      v8::Local<v8::Object>::New(isolate, function_data_);
  function_data->Delete(v8::String::NewFromUtf8(isolate,
                                                kXWalkExtensionModule));

  object_template_.Reset();
  function_data_.Reset();
  message_listener_.Reset();

  if (!instance_id_.empty())
    client_->DestroyInstance(instance_id_);
}

namespace {

std::string CodeToEnsureNamespace(const std::string& extension_name) {
  std::string result;
  size_t pos = 0;
  while (true) {
    pos = extension_name.find('.', pos);
    if (pos == std::string::npos) {
      result += extension_name + " = {};";
      break;
    }
    std::string ns = extension_name.substr(0, pos);
    result += ns + " = " + ns + " || {}; ";
    pos++;
  }
  return result;
}

// Templatized backend for StringPrintF/StringAppendF. This does not finalize
// the va_list, the caller is expected to do that.
template <class StringType>
static void StringAppendVT(StringType* dst,
                           const typename StringType::value_type* format,
                           va_list ap) {
  // First try with a small fixed size buffer.
  // This buffer size should be kept in sync with StringUtilTest.GrowBoundary
  // and StringUtilTest.StringPrintfBounds.
  typename StringType::value_type stack_buf[1024];

  va_list ap_copy;
  va_copy(ap_copy, ap);

  int result = vsnprintf(stack_buf, arraysize(stack_buf), format, ap_copy);
  va_end(ap_copy);

  if (result >= 0 && result < static_cast<int>(arraysize(stack_buf))) {
    // It fit.
    dst->append(stack_buf, result);
    return;
  }

  // Repeatedly increase buffer size until it fits.
  int mem_length = arraysize(stack_buf);
  while (true) {
    if (result < 0) {
      if (errno != 0 && errno != EOVERFLOW)
        return;
      // Try doubling the buffer size.
      mem_length *= 2;
    } else {
      // We need exactly "result + 1" characters.
      mem_length = result + 1;
    }

    if (mem_length > 32 * 1024 * 1024) {
      // That should be plenty, don't try anything larger.  This protects
      // against huge allocations when using vsnprintfT implementations that
      // return -1 for reasons other than overflow without setting errno.
      LOGE("Unable to printf the requested string due to size.");
      return;
    }

    std::vector<typename StringType::value_type> mem_buf(mem_length);

    // NOTE: You can only use a va_list once.  Since we're in a while loop, we
    // need to make a new copy each time so we don't use up the original.
    va_copy(ap_copy, ap);
    result = vsnprintf(&mem_buf[0], mem_length, format, ap_copy);
    va_end(ap_copy);

    if ((result >= 0) && (result < mem_length)) {
      // It fit.
      dst->append(&mem_buf[0], result);
      return;
    }
  }
}

std::string StringPrintf(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  std::string result;
  StringAppendVT(&result, format, ap);
  va_end(ap);
  return result;
}

// Wrap API code into a callable form that takes extension object as parameter.
std::string WrapAPICode(const std::string& extension_code,
                        const std::string& extension_name) {
  // We take care here to make sure that line numbering for api_code after
  // wrapping doesn't change, so that syntax errors point to the correct line.

  return StringPrintf(
      "var %s; (function(extension, requireNative) { "
      "extension.internal = {};"
      "extension.internal.sendSyncMessage = extension.sendSyncMessage;"
      "delete extension.sendSyncMessage;"
      "var Object = requireNative('objecttools');"
      "var exports = {}; (function() {'use strict'; %s\n})();"
      "%s = exports; });",
      CodeToEnsureNamespace(extension_name).c_str(),
      extension_code.c_str(),
      extension_name.c_str());
}

std::string ExceptionToString(const v8::TryCatch& try_catch) {
  std::string str;
  v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
  v8::String::Utf8Value exception(try_catch.Exception());
  v8::Local<v8::Message> message(try_catch.Message());
  if (message.IsEmpty()) {
    str.append(StringPrintf("%s\n", *exception));
  } else {
    v8::String::Utf8Value filename(message->GetScriptResourceName());
    int linenum = message->GetLineNumber();
    int colnum = message->GetStartColumn();
    str.append(StringPrintf(
        "%s:%i:%i %s\n", *filename, linenum, colnum, *exception));
    v8::String::Utf8Value sourceline(message->GetSourceLine());
    str.append(StringPrintf("%s\n", *sourceline));
  }
  return str;
}

v8::Handle<v8::Value> RunString(const std::string& code,
                                std::string* exception) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::EscapableHandleScope handle_scope(isolate);
  v8::Handle<v8::String> v8_code(
      v8::String::NewFromUtf8(isolate, code.c_str()));

  v8::TryCatch try_catch;
  try_catch.SetVerbose(true);

  v8::Handle<v8::Script> script(v8::Script::Compile(v8_code));
  if (try_catch.HasCaught()) {
    *exception = ExceptionToString(try_catch);
    return handle_scope.Escape(
        v8::Local<v8::Primitive>(v8::Undefined(isolate)));
  }

  v8::Local<v8::Value> result = script->Run();
  if (try_catch.HasCaught()) {
    *exception = ExceptionToString(try_catch);
    return handle_scope.Escape(
        v8::Local<v8::Primitive>(v8::Undefined(isolate)));
  }

  return handle_scope.Escape(result);
}

}  // namespace

void XWalkExtensionModule::LoadExtensionCode(
    v8::Handle<v8::Context> context, v8::Handle<v8::Function> require_native) {
  instance_id_ = client_->CreateInstance(extension_name_, this);
  if (instance_id_.empty()) {
    LOGGER(ERROR) << "Failed to create an instance of " << extension_name_;
    return;
  }



  XWalkExtension* ext = client_->GetExtension(extension_name_);
  if (ext == nullptr) {
    LOGGER(ERROR) << "Failed to get an extension " << extension_name_;
    return;
  }
  std::string wrapped_api_code =
      WrapAPICode(ext->GetJavascriptCode(), extension_name_);

  std::string exception;
  v8::Handle<v8::Value> result = RunString(wrapped_api_code, &exception);

  if (!result->IsFunction()) {
    LOGGER(ERROR) << "Couldn't load JS API code for "
                  << extension_name_ << " : " << exception;
    return;
  }
  v8::Handle<v8::Function> callable_api_code =
      v8::Handle<v8::Function>::Cast(result);
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::Local<v8::ObjectTemplate>::New(context->GetIsolate(),
                                          object_template_);

  const int argc = 2;
  v8::Handle<v8::Value> argv[argc] = {
    object_template->NewInstance(),
    require_native
  };

  v8::TryCatch try_catch;
  try_catch.SetVerbose(true);
  callable_api_code->Call(context->Global(), argc, argv);
  if (try_catch.HasCaught()) {
    LOGGER(ERROR) << "Exception while loading JS API code for "
                  << extension_name_ << " : " << ExceptionToString(try_catch);
  }
}

void XWalkExtensionModule::HandleMessageFromNative(const std::string& msg) {
  if (message_listener_.IsEmpty())
    return;

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::Context> context = module_system_->GetV8Context();
  v8::Context::Scope context_scope(context);

  v8::Handle<v8::Value> args[] = {
      v8::String::NewFromUtf8(isolate, msg.c_str()) };

  v8::Handle<v8::Function> message_listener =
      v8::Local<v8::Function>::New(isolate, message_listener_);

  v8::TryCatch try_catch;
  message_listener->Call(context->Global(), 1, args);
  if (try_catch.HasCaught())
    LOGGER(ERROR) << "Exception when running message listener: "
                  << ExceptionToString(try_catch);
}

// static
void XWalkExtensionModule::PostMessageCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::ReturnValue<v8::Value> result(info.GetReturnValue());
  XWalkExtensionModule* module = GetExtensionModule(info);
  if (!module || info.Length() != 1) {
    result.Set(false);
    return;
  }

  v8::String::Utf8Value value(info[0]->ToString());

  // CHECK(module->instance_id_);
  module->client_->PostMessageToNative(module->instance_id_,
                                       std::string(*value));
  result.Set(true);
}

// static
void XWalkExtensionModule::SendSyncMessageCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::ReturnValue<v8::Value> result(info.GetReturnValue());
  XWalkExtensionModule* module = GetExtensionModule(info);
  if (!module || info.Length() != 1) {
    result.Set(false);
    return;
  }

  v8::String::Utf8Value value(info[0]->ToString());

  // CHECK(module->instance_id_);
  std::string reply =
      module->client_->SendSyncMessageToNative(module->instance_id_,
                                               std::string(*value));

  // If we tried to send a message to an instance that became invalid,
  // then reply will be NULL.
  if (!reply.empty()) {
    result.Set(v8::String::NewFromUtf8(info.GetIsolate(), reply.c_str()));
  }
}

// static
void XWalkExtensionModule::SetMessageListenerCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::ReturnValue<v8::Value> result(info.GetReturnValue());
  XWalkExtensionModule* module = GetExtensionModule(info);
  if (!module || info.Length() != 1) {
    result.Set(false);
    return;
  }

  if (!info[0]->IsFunction() && !info[0]->IsUndefined()) {
    LOGGER(ERROR) << "Trying to set message listener with invalid value.";
    result.Set(false);
    return;
  }

  v8::Isolate* isolate = info.GetIsolate();
  if (info[0]->IsUndefined())
    module->message_listener_.Reset();
  else
    module->message_listener_.Reset(isolate, info[0].As<v8::Function>());

  result.Set(true);
}

// static
void XWalkExtensionModule::SendRuntimeMessageCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::ReturnValue<v8::Value> result(info.GetReturnValue());
  XWalkExtensionModule* module = GetExtensionModule(info);
  if (!module || info.Length() < 1) {
    result.Set(false);
    return;
  }

  v8::String::Utf8Value type(info[0]->ToString());
  std::string data_str;
  if (info.Length() > 1) {
    v8::String::Utf8Value data(info[1]->ToString());
    data_str = std::string(*data);
  }

  RuntimeIPCClient* rc = RuntimeIPCClient::GetInstance();
  rc->SendMessage(module->module_system_->GetV8Context(),
                  std::string(*type), data_str);

  result.Set(true);
}

// static
void XWalkExtensionModule::SendRuntimeSyncMessageCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();

  v8::ReturnValue<v8::Value> result(info.GetReturnValue());
  XWalkExtensionModule* module = GetExtensionModule(info);
  if (!module || info.Length() < 1) {
    result.SetUndefined();
    return;
  }

  v8::String::Utf8Value type(info[0]->ToString());
  std::string data_str;
  if (info.Length() > 1) {
    v8::String::Utf8Value data(info[1]->ToString());
    data_str = std::string(*data);
  }

  RuntimeIPCClient* rc = RuntimeIPCClient::GetInstance();
  std::string reply = rc->SendSyncMessage(
      module->module_system_->GetV8Context(),
      std::string(*type), data_str);

  result.Set(v8::String::NewFromUtf8(isolate, reply.c_str()));
}

// static
void XWalkExtensionModule::SendRuntimeAsyncMessageCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::ReturnValue<v8::Value> result(info.GetReturnValue());
  XWalkExtensionModule* module = GetExtensionModule(info);
  if (!module || info.Length() < 1) {
    result.Set(false);
    return;
  }

  // type
  v8::String::Utf8Value type(info[0]->ToString());

  // value
  std::string value_str;
  if (info.Length() > 1) {
    v8::String::Utf8Value value(info[1]->ToString());
    value_str = std::string(*value);
  }

  // callback
  RuntimeIPCClient::JSCallback* js_callback = NULL;
  if (info.Length() > 2) {
    if (info[2]->IsFunction()) {
      v8::Handle<v8::Function> func = info[2].As<v8::Function>();
      js_callback = new RuntimeIPCClient::JSCallback(isolate, func);
    }
  }

  auto callback = [js_callback](const std::string& /*type*/,
                     const std::string& value) -> void {
    if (!js_callback) {
      LOGGER(ERROR) << "JsCallback is NULL.";
      return;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handle_scope(isolate);
    v8::Handle<v8::Value> args[] = {
        v8::String::NewFromUtf8(isolate, value.c_str()) };
    js_callback->Call(isolate, args);
    delete js_callback;
  };

  RuntimeIPCClient* rc = RuntimeIPCClient::GetInstance();
  rc->SendAsyncMessage(module->module_system_->GetV8Context(),
                       std::string(*type), value_str, callback);

  result.Set(true);
}

// static
XWalkExtensionModule* XWalkExtensionModule::GetExtensionModule(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::Object> data = info.Data().As<v8::Object>();
  v8::Local<v8::Value> module =
      data->Get(v8::String::NewFromUtf8(isolate, kXWalkExtensionModule));
  if (module.IsEmpty() || module->IsUndefined()) {
    LOGGER(ERROR) << "Trying to use extension from already destroyed context!";
    return NULL;
  }
  // CHECK(module->IsExternal());
  return static_cast<XWalkExtensionModule*>(module.As<v8::External>()->Value());
}

}  // namespace extensions
