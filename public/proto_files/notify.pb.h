// Generated by the protocol buffer compiler.  DO NOT EDIT!
// source: notify.proto

#ifndef GOOGLE_PROTOBUF_INCLUDED_notify_2eproto
#define GOOGLE_PROTOBUF_INCLUDED_notify_2eproto

#include <limits>
#include <string>

#include <google/protobuf/port_def.inc>
#if PROTOBUF_VERSION < 3021000
#error This file was generated by a newer version of protoc which is
#error incompatible with your Protocol Buffer headers. Please update
#error your headers.
#endif
#if 3021012 < PROTOBUF_MIN_PROTOC_VERSION
#error This file was generated by an older version of protoc which is
#error incompatible with your Protocol Buffer headers. Please
#error regenerate this file with a newer version of protoc.
#endif

#include <google/protobuf/port_undef.inc>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/arena.h>
#include <google/protobuf/arenastring.h>
#include <google/protobuf/generated_message_util.h>
#include <google/protobuf/metadata_lite.h>
#include <google/protobuf/generated_message_reflection.h>
#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>  // IWYU pragma: export
#include <google/protobuf/extension_set.h>  // IWYU pragma: export
#include <google/protobuf/unknown_field_set.h>
#include "base.pb.h"
// @@protoc_insertion_point(includes)
#include <google/protobuf/port_def.inc>
#define PROTOBUF_INTERNAL_EXPORT_notify_2eproto
PROTOBUF_NAMESPACE_OPEN
namespace internal {
class AnyMetadata;
}  // namespace internal
PROTOBUF_NAMESPACE_CLOSE

// Internal implementation detail -- do not use these members.
struct TableStruct_notify_2eproto {
  static const uint32_t offsets[];
};
extern const ::PROTOBUF_NAMESPACE_ID::internal::DescriptorTable descriptor_table_notify_2eproto;
namespace cs {
class Notify;
struct NotifyDefaultTypeInternal;
extern NotifyDefaultTypeInternal _Notify_default_instance_;
}  // namespace cs
PROTOBUF_NAMESPACE_OPEN
template<> ::cs::Notify* Arena::CreateMaybeMessage<::cs::Notify>(Arena*);
PROTOBUF_NAMESPACE_CLOSE
namespace cs {

// ===================================================================

class Notify final :
    public ::PROTOBUF_NAMESPACE_ID::Message /* @@protoc_insertion_point(class_definition:cs.Notify) */ {
 public:
  inline Notify() : Notify(nullptr) {}
  ~Notify() override;
  explicit PROTOBUF_CONSTEXPR Notify(::PROTOBUF_NAMESPACE_ID::internal::ConstantInitialized);

  Notify(const Notify& from);
  Notify(Notify&& from) noexcept
    : Notify() {
    *this = ::std::move(from);
  }

  inline Notify& operator=(const Notify& from) {
    CopyFrom(from);
    return *this;
  }
  inline Notify& operator=(Notify&& from) noexcept {
    if (this == &from) return *this;
    if (GetOwningArena() == from.GetOwningArena()
  #ifdef PROTOBUF_FORCE_COPY_IN_MOVE
        && GetOwningArena() != nullptr
  #endif  // !PROTOBUF_FORCE_COPY_IN_MOVE
    ) {
      InternalSwap(&from);
    } else {
      CopyFrom(from);
    }
    return *this;
  }

  static const ::PROTOBUF_NAMESPACE_ID::Descriptor* descriptor() {
    return GetDescriptor();
  }
  static const ::PROTOBUF_NAMESPACE_ID::Descriptor* GetDescriptor() {
    return default_instance().GetMetadata().descriptor;
  }
  static const ::PROTOBUF_NAMESPACE_ID::Reflection* GetReflection() {
    return default_instance().GetMetadata().reflection;
  }
  static const Notify& default_instance() {
    return *internal_default_instance();
  }
  static inline const Notify* internal_default_instance() {
    return reinterpret_cast<const Notify*>(
               &_Notify_default_instance_);
  }
  static constexpr int kIndexInFileMessages =
    0;

  friend void swap(Notify& a, Notify& b) {
    a.Swap(&b);
  }
  inline void Swap(Notify* other) {
    if (other == this) return;
  #ifdef PROTOBUF_FORCE_COPY_IN_SWAP
    if (GetOwningArena() != nullptr &&
        GetOwningArena() == other->GetOwningArena()) {
   #else  // PROTOBUF_FORCE_COPY_IN_SWAP
    if (GetOwningArena() == other->GetOwningArena()) {
  #endif  // !PROTOBUF_FORCE_COPY_IN_SWAP
      InternalSwap(other);
    } else {
      ::PROTOBUF_NAMESPACE_ID::internal::GenericSwap(this, other);
    }
  }
  void UnsafeArenaSwap(Notify* other) {
    if (other == this) return;
    GOOGLE_DCHECK(GetOwningArena() == other->GetOwningArena());
    InternalSwap(other);
  }

  // implements Message ----------------------------------------------

  Notify* New(::PROTOBUF_NAMESPACE_ID::Arena* arena = nullptr) const final {
    return CreateMaybeMessage<Notify>(arena);
  }
  using ::PROTOBUF_NAMESPACE_ID::Message::CopyFrom;
  void CopyFrom(const Notify& from);
  using ::PROTOBUF_NAMESPACE_ID::Message::MergeFrom;
  void MergeFrom( const Notify& from) {
    Notify::MergeImpl(*this, from);
  }
  private:
  static void MergeImpl(::PROTOBUF_NAMESPACE_ID::Message& to_msg, const ::PROTOBUF_NAMESPACE_ID::Message& from_msg);
  public:
  PROTOBUF_ATTRIBUTE_REINITIALIZES void Clear() final;
  bool IsInitialized() const final;

  size_t ByteSizeLong() const final;
  const char* _InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) final;
  uint8_t* _InternalSerialize(
      uint8_t* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const final;
  int GetCachedSize() const final { return _impl_._cached_size_.Get(); }

  private:
  void SharedCtor(::PROTOBUF_NAMESPACE_ID::Arena* arena, bool is_message_owned);
  void SharedDtor();
  void SetCachedSize(int size) const final;
  void InternalSwap(Notify* other);

  private:
  friend class ::PROTOBUF_NAMESPACE_ID::internal::AnyMetadata;
  static ::PROTOBUF_NAMESPACE_ID::StringPiece FullMessageName() {
    return "cs.Notify";
  }
  protected:
  explicit Notify(::PROTOBUF_NAMESPACE_ID::Arena* arena,
                       bool is_message_owned = false);
  public:

  static const ClassData _class_data_;
  const ::PROTOBUF_NAMESPACE_ID::Message::ClassData*GetClassData() const final;

  ::PROTOBUF_NAMESPACE_ID::Metadata GetMetadata() const final;

  // nested types ----------------------------------------------------

  // accessors -------------------------------------------------------

  enum : int {
    kChatMessageFieldNumber = 1,
  };
  // .cs.ChatMessage chat_message = 1;
  bool has_chat_message() const;
  private:
  bool _internal_has_chat_message() const;
  public:
  void clear_chat_message();
  const ::cs::ChatMessage& chat_message() const;
  PROTOBUF_NODISCARD ::cs::ChatMessage* release_chat_message();
  ::cs::ChatMessage* mutable_chat_message();
  void set_allocated_chat_message(::cs::ChatMessage* chat_message);
  private:
  const ::cs::ChatMessage& _internal_chat_message() const;
  ::cs::ChatMessage* _internal_mutable_chat_message();
  public:
  void unsafe_arena_set_allocated_chat_message(
      ::cs::ChatMessage* chat_message);
  ::cs::ChatMessage* unsafe_arena_release_chat_message();

  // @@protoc_insertion_point(class_scope:cs.Notify)
 private:
  class _Internal;

  template <typename T> friend class ::PROTOBUF_NAMESPACE_ID::Arena::InternalHelper;
  typedef void InternalArenaConstructable_;
  typedef void DestructorSkippable_;
  struct Impl_ {
    ::cs::ChatMessage* chat_message_;
    mutable ::PROTOBUF_NAMESPACE_ID::internal::CachedSize _cached_size_;
  };
  union { Impl_ _impl_; };
  friend struct ::TableStruct_notify_2eproto;
};
// ===================================================================


// ===================================================================

#ifdef __GNUC__
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif  // __GNUC__
// Notify

// .cs.ChatMessage chat_message = 1;
inline bool Notify::_internal_has_chat_message() const {
  return this != internal_default_instance() && _impl_.chat_message_ != nullptr;
}
inline bool Notify::has_chat_message() const {
  return _internal_has_chat_message();
}
inline const ::cs::ChatMessage& Notify::_internal_chat_message() const {
  const ::cs::ChatMessage* p = _impl_.chat_message_;
  return p != nullptr ? *p : reinterpret_cast<const ::cs::ChatMessage&>(
      ::cs::_ChatMessage_default_instance_);
}
inline const ::cs::ChatMessage& Notify::chat_message() const {
  // @@protoc_insertion_point(field_get:cs.Notify.chat_message)
  return _internal_chat_message();
}
inline void Notify::unsafe_arena_set_allocated_chat_message(
    ::cs::ChatMessage* chat_message) {
  if (GetArenaForAllocation() == nullptr) {
    delete reinterpret_cast<::PROTOBUF_NAMESPACE_ID::MessageLite*>(_impl_.chat_message_);
  }
  _impl_.chat_message_ = chat_message;
  if (chat_message) {
    
  } else {
    
  }
  // @@protoc_insertion_point(field_unsafe_arena_set_allocated:cs.Notify.chat_message)
}
inline ::cs::ChatMessage* Notify::release_chat_message() {
  
  ::cs::ChatMessage* temp = _impl_.chat_message_;
  _impl_.chat_message_ = nullptr;
#ifdef PROTOBUF_FORCE_COPY_IN_RELEASE
  auto* old =  reinterpret_cast<::PROTOBUF_NAMESPACE_ID::MessageLite*>(temp);
  temp = ::PROTOBUF_NAMESPACE_ID::internal::DuplicateIfNonNull(temp);
  if (GetArenaForAllocation() == nullptr) { delete old; }
#else  // PROTOBUF_FORCE_COPY_IN_RELEASE
  if (GetArenaForAllocation() != nullptr) {
    temp = ::PROTOBUF_NAMESPACE_ID::internal::DuplicateIfNonNull(temp);
  }
#endif  // !PROTOBUF_FORCE_COPY_IN_RELEASE
  return temp;
}
inline ::cs::ChatMessage* Notify::unsafe_arena_release_chat_message() {
  // @@protoc_insertion_point(field_release:cs.Notify.chat_message)
  
  ::cs::ChatMessage* temp = _impl_.chat_message_;
  _impl_.chat_message_ = nullptr;
  return temp;
}
inline ::cs::ChatMessage* Notify::_internal_mutable_chat_message() {
  
  if (_impl_.chat_message_ == nullptr) {
    auto* p = CreateMaybeMessage<::cs::ChatMessage>(GetArenaForAllocation());
    _impl_.chat_message_ = p;
  }
  return _impl_.chat_message_;
}
inline ::cs::ChatMessage* Notify::mutable_chat_message() {
  ::cs::ChatMessage* _msg = _internal_mutable_chat_message();
  // @@protoc_insertion_point(field_mutable:cs.Notify.chat_message)
  return _msg;
}
inline void Notify::set_allocated_chat_message(::cs::ChatMessage* chat_message) {
  ::PROTOBUF_NAMESPACE_ID::Arena* message_arena = GetArenaForAllocation();
  if (message_arena == nullptr) {
    delete reinterpret_cast< ::PROTOBUF_NAMESPACE_ID::MessageLite*>(_impl_.chat_message_);
  }
  if (chat_message) {
    ::PROTOBUF_NAMESPACE_ID::Arena* submessage_arena =
        ::PROTOBUF_NAMESPACE_ID::Arena::InternalGetOwningArena(
                reinterpret_cast<::PROTOBUF_NAMESPACE_ID::MessageLite*>(chat_message));
    if (message_arena != submessage_arena) {
      chat_message = ::PROTOBUF_NAMESPACE_ID::internal::GetOwnedMessage(
          message_arena, chat_message, submessage_arena);
    }
    
  } else {
    
  }
  _impl_.chat_message_ = chat_message;
  // @@protoc_insertion_point(field_set_allocated:cs.Notify.chat_message)
}

#ifdef __GNUC__
  #pragma GCC diagnostic pop
#endif  // __GNUC__

// @@protoc_insertion_point(namespace_scope)

}  // namespace cs

// @@protoc_insertion_point(global_scope)

#include <google/protobuf/port_undef.inc>
#endif  // GOOGLE_PROTOBUF_INCLUDED_GOOGLE_PROTOBUF_INCLUDED_notify_2eproto
