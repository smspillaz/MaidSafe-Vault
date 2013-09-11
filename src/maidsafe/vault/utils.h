/*  Copyright 2012 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.novinet.com/license

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#ifndef MAIDSAFE_VAULT_UTILS_H_
#define MAIDSAFE_VAULT_UTILS_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "boost/filesystem/path.hpp"
#include "leveldb/db.h"

#include "maidsafe/common/error.h"
#include "maidsafe/data_types/data_name_variant.h"
#include "maidsafe/routing/routing_api.h"
#include "maidsafe/routing/parameters.h"

#include "maidsafe/nfs/message_types.h"
#include "maidsafe/nfs/client/messages.h"

#include "maidsafe/vault/types.h"
#include "maidsafe/vault/data_manager/data_manager.h"
#include "maidsafe/vault/version_manager/version_manager.h"


namespace maidsafe {

namespace vault {

template<typename T>
class Accumulator;

namespace detail {

template<typename T>
struct RequiredValue {};

template<>
struct RequiredValue<routing::SingleSource> {
  static const int value = 1;
};

template<>
struct RequiredValue<routing::GroupSource> {
  static const int value = routing::Parameters::node_group_size - 1;
};

class GetSenderVisitor : public boost::static_visitor<NodeId> {
 public:
  template<typename SenderType>
  result_type operator()(const SenderType& sender) {
    return sender.data;
  }
};

template<>
GetSenderVisitor::result_type GetSenderVisitor::operator()(const routing::GroupSource& source) {
  return source.sender_id.data;
}

template<typename MessageType, typename ServiceHandlerType>
void DoOperation(const MessageType& message, ServiceHandlerType* service, const NodeId& node_id);

template <typename ValidateSender,
          typename AccumulatorType,
          typename Checker,
          typename ServiceHandlerType>
struct OperationHandler {
 public:
  OperationHandler(ValidateSender validate_sender_in,
                   AccumulatorType& accumulator_in,
                   Checker checker_in,
                   ServiceHandlerType* service,
                   std::mutex& mutex_in)
      : validate_sender(validate_sender_in),
        accumulator(accumulator_in),
        checker(checker_in),
        service_(service),
        mutex(mutex_in) {}

  template<typename MessageType, typename Sender, typename Receiver>
  void operator() (const MessageType& message, const Sender& sender, const Receiver& /*receiver*/) {
    if (!validate_sender(message, sender))
      return;
    {
      std::lock_guard<std::mutex> lock(mutex);
        if (accumulator.CheckHandled(message))
          return;
      if (accumulator.AddPendingRequest(message, sender, checker) !=
              Accumulator<typename AccumulatorType::type>::AddResult::kSuccess)
        return;
    }
    DoOperation<MessageType, ServiceHandlerType>(message,
                                                 service_,
                                                 boost::apply_visitor(GetSenderVisitor(), sender));
  }

 private:
  ValidateSender validate_sender;
  AccumulatorType& accumulator;
  Checker checker;
  ServiceHandlerType* service_;
  std::mutex& mutex;
};

template<typename MessageType, typename ServiceHandlerType>
void DoOperation(const MessageType& /*message*/,
                 ServiceHandlerType* /*service*/,
                 const NodeId& node_id) {
  MessageType::Invalid_function_call;
}


// ================================== Account Specialisations ====================================
// CreateAccountRequestFromMaidNodeToMaidManager, Empty

template<typename ServiceHandlerType>
void DoOperation(const nfs::CreateAccountRequestFromMaidNodeToMaidManager& /*message*/,
                 ServiceHandlerType* service,
                 const NodeId& sender) {
  service->CreateAccount(MaidName(sender));
}


// ================================== Delete Specialisations ======================================
//   DeleteRequestFromMaidNodeToMaidManager, DataName
//   DeleteRequestFromMaidManagerToDataManager, DataName
//   DeleteRequestFromDataManagerToPmidManager, DataName
//   DeleteRequestFromPmidManagerToPmidNode, DataName

template <typename ServiceHandlerType>
class DeleteVisitor : public boost::static_visitor<> {
 public:
  DeleteVisitor(ServiceHandlerType* service, const NodeId& sender, const nfs::MessageId& message_id)
      : service_(service),
        kSender(sender),
        kMessageId(message_id) {}

  template <typename Name>
  void operator()(const Name& data_name) {
    service_.HandleDelete<Name::data_type>(kSender, data_name, kMessageId);
  }
 private:
  ServiceHandlerType* service_;
  NodeId kSender;
  nfs::MessageId kMessageId;
};

template<typename ServiceHandlerType>
void DoOperation(const nfs::DeleteRequestFromMaidNodeToMaidManager& message,
                 ServiceHandlerType* service,
                 const NodeId& sender) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  DeleteVisitor<ServiceHandlerType> delete_visitor(service, sender, message.message_id);
  boost::apply_visitor(delete_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::DeleteRequestFromMaidManagerToDataManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  DeleteVisitor<ServiceHandlerType> delete_visitor(service);
  boost::apply_visitor(delete_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::DeleteRequestFromDataManagerToPmidManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  DeleteVisitor<ServiceHandlerType> delete_visitor(service);
  boost::apply_visitor(delete_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::DeleteRequestFromPmidManagerToPmidNode& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  DeleteVisitor<ServiceHandlerType> delete_visitor(service);
  boost::apply_visitor(delete_visitor, data_name);
}

// ================================== Get Specialisations ======================================
//   GetCachedResponseFromPmidNodeToMaidNode, DataNameAndContentOrReturnCode
//   GetVersionsResponseFromVersionManagerToMaidNode, StructuredDataNameAndContentOrReturnCode
//   GetBranchResponseFromVersionManagerToMaidNode, StructuredDataNameAndContentOrReturnCode
//   GetVersionsResponseFromVersionManagerToDataGetter, StructuredDataNameAndContentOrReturnCode
//   GetBranchResponseFromVersionManagerToDataGetter, StructuredDataNameAndContentOrReturnCode
//   GetPmidAccountRequestFromPmidNodeToPmidManager, Empty
//   GetPmidHealthRequestFromMaidNodeToMaidManager, DataName
//   GetVersionsRequestFromMaidNodeToVersionManager, DataName
//   GetBranchRequestFromMaidNodeToVersionManager, DataNameAndVersion
//   GetVersionsRequestFromDataGetterToVersionManager, DataName
//   GetBranchRequestFromDataGetterToVersionManager, DataNameAndVersion
//   GetPmidAccountResponseFromPmidManagerToPmidNode, DataNameAndContentOrReturnCode


// ================================= Get Response Specialisations ================================
//   GetResponseFromDataManagerToMaidNode, DataNameAndContentOrReturnCode
//   GetResponseFromDataManagerToDataGetter, DataNameAndContentOrReturnCode
//   GetResponseFromPmidNodeToDataManager, DataNameAndContentOrReturnCode

template <typename ServiceHandlerType>
class GetResponseVisitor : public boost::static_visitor<> {
 public:
  GetResponseVisitor(ServiceHandlerType* service, const NonEmptyString& content)
      : service_(service),
        content_(content),
        error_(CommonErrors::success) {}

  GetResponseVisitor(ServiceHandlerType* service, const maidsafe_error& error)
      : service_(service),
        error_(error) {}

  template <typename Name>
  void operator()(const Name& data_name) {
    if (error_.code() == CommonErrors::success)
      service_.HandleGetResponse<Name::data_type>(data_name, content_);
    else
      service_.HandleGetResponse<Name::data_type>(data_name, error_);
  }
  private:
   ServiceHandlerType* service_;
   NonEmptyString content_;
   maidsafe_error error_;
};

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetResponseFromDataManagerToMaidNode& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  if (message.contents->data) {
    auto data_name(GetDataNameVariant(message.contents->data->name.type,
                                      message.contents->data->name.raw_name));
    GetResponseVisitor<ServiceHandlerType> get_response_visitor(service,
                                                            message.contents->data->content);
    boost::apply_visitor(get_response_visitor, data_name);
  } else {
    auto data_name(GetDataNameVariant(
        message.contents->data_name_and_return_code->name.type,
        message.contents->data_name_and_return_code->name.raw_name));
    GetResponseVisitor<ServiceHandlerType> get_response_visitor(
        service, message.contents->data_name_and_return_code->return_code);
    boost::apply_visitor(get_response_visitor, data_name);
  }
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetResponseFromDataManagerToDataGetter& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  if (message.contents->data) {
    auto data_name(GetDataNameVariant(message.contents->data->name.type,
                                      message.contents->data->name.raw_name));
    GetResponseVisitor<ServiceHandlerType> get_response_visitor(service,
                                                         message.contents->data->content);
    boost::apply_visitor(get_response_visitor, data_name);
  } else {
    auto data_name(GetDataNameVariant(
        message.contents->data_name_and_return_code->name.type,
        message.contents->data_name_and_return_code->name.raw_name));
    GetResponseVisitor<ServiceHandlerType> get_response_visitor(
        service, message.contents->data_name_and_return_code->return_code);
    boost::apply_visitor(get_response_visitor, data_name);
  }
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetResponseFromPmidNodeToDataManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  if (message.contents->data) {
    auto data_name(GetDataNameVariant(message.contents->data->name.type,
                                      message.contents->data->name.raw_name));
    GetResponseVisitor<ServiceHandlerType> get_response_visitor(service,
                                                         message.contents->data->content);
    boost::apply_visitor(get_response_visitor, data_name);
  } else {
    auto data_name(GetDataNameVariant(
        message.contents->data_name_and_return_code->name.type,
        message.contents->data_name_and_return_code->name.raw_name));
    GetResponseVisitor<ServiceHandlerType> get_response_visitor(
        service, message.contents->data_name_and_return_code->return_code);
    boost::apply_visitor(get_response_visitor, data_name);
  }
}

// ================================== Get Request Specialisations =================================
//   GetRequestFromMaidNodeToDataManager, DataName
//   GetRequestFromDataManagerToPmidNode, DataName
//   GetRequestFromDataGetterToDataManager, DataName
//   GetRequestFromPmidNodeToDataManager, DataName

template <typename ServiceType>
class GetRequestVisitor : public boost::static_visitor<> {
 public:
  GetRequestVisitor(ServiceType& service)
      : service_(service) {}

  template <typename Name>
  void operator()(const Name& data_name) {
    service_.HandleGet<Name::data_type>(data_name);
  }
  private:
   ServiceType& service_;
};

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetRequestFromMaidNodeToDataManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  GetRequestVisitor<ServiceHandlerType> get_visitor(service);
  boost::apply_visitor(get_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetRequestFromPmidNodeToDataManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  GetRequestVisitor<ServiceHandlerType> get_visitor(service);
  boost::apply_visitor(get_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetRequestFromDataManagerToPmidNode& message,
                 ServiceHandlerType* service) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  GetRequestVisitor<ServiceHandlerType> get_visitor(service);
  boost::apply_visitor(get_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::GetRequestFromDataGetterToDataManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->type, message.contents->raw_name));
  GetRequestVisitor<ServiceHandlerType> get_visitor(service);
  boost::apply_visitor(get_visitor, data_name);
}

// ================================== Put Specialisations ======================================

template <typename ServiceHandlerType>
class HintedPutVisitor : public boost::static_visitor<> {
 public:
  HintedPutVisitor(ServiceHandlerType* service,
                   const NonEmptyString& content,
                   const NodeId& sender,
                   const Identity pmid_hint,
                   const nfs::MessageId& message_id)
      : service_(service),
        content_(content),
        kSender(sender),
        pmid_hint_(pmid_hint),
        kMessageId(message_id) {}

  template <typename Name>
  void operator()(const Name& data_name) {
    service_.HandlePut(MaidName(kSender),
                       Name::data_type(data_name, content_),
                       pmid_hint_,
                       kMessageId);
  }
  private:
   ServiceHandlerType* service_;
   NonEmptyString content_;
   Identity pmid_hint_;
   nfs::MessageId kMessageId;
   NodeId kSender;
};

template <typename ServiceHandlerType>
class PutVisitor : public boost::static_visitor<> {
 public:
  PutVisitor(ServiceHandlerType* service,
             const NonEmptyString& content)
      : service_(service),
        content_(content) {}

  template <typename Name>
  void operator()(const Name& data_name) {
    service_.HandlePut(Name::data_type(data_name, content_));
  }
 private:
  ServiceHandlerType* service_;
  NonEmptyString content_;
};


template<typename ServiceHandlerType>
void DoOperation(const nfs::PutRequestFromDataManagerToPmidManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->name.type,
                                    message.contents->name.raw_name));
  PutVisitor<ServiceHandlerType> put_visitor(service, message.contents->content);
  boost::apply_visitor(put_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::PutRequestFromPmidManagerToPmidNode& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->name.type,
                                    message.contents->name.raw_name));
  PutVisitor<ServiceHandlerType> put_visitor(service, message.contents->content);
  boost::apply_visitor(put_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::PutRequestFromMaidManagerToDataManager& message,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
   auto data_name(GetDataNameVariant(message.contents->data.name.type,
                                     message.contents->data.name.raw_name));
  HintedPutVisitor<ServiceHandlerType> put_visitor(service,
                                                   message.contents->data.content,
                                                   message.contents->pmid_hint);
  boost::apply_visitor(put_visitor, data_name);
}

template<typename ServiceHandlerType>
void DoOperation(const nfs::PutRequestFromMaidNodeToMaidManager& message,
                 const NodeId& sender,
                 ServiceHandlerType* service,
                 const NodeId& /*node_id*/) {
  auto data_name(GetDataNameVariant(message.contents->data.name.type,
                                    message.contents->data.name.raw_name));
  HintedPutVisitor<ServiceHandlerType> put_visitor(service,
                                                   message.contents->data.content,
                                                   sender,
                                                   message.contents->pmid_hint,
                                                   message.message_id);
  boost::apply_visitor(put_visitor, data_name);
}


// =============================================================================================

template <typename MessageType>
struct ValidateSenderType {
  typedef std::function<bool(const MessageType&, const typename MessageType::Sender&)> type;
};


void InitialiseDirectory(const boost::filesystem::path& directory);
//bool ShouldRetry(routing::Routing& routing, const nfs::Message& message);

template<typename Data>
bool IsDataElement(const typename Data::Name& name, const DataNameVariant& data_name_variant);

//void SendReply(const nfs::Message& original_message,
//               const maidsafe_error& return_code,
//               const routing::ReplyFunctor& reply_functor);

template<typename AccountSet, typename Account>
typename Account::serialised_type GetSerialisedAccount(
    std::mutex& mutex,
    const AccountSet& accounts,
    const typename Account::Name& account_name);

template<typename AccountSet, typename Account>
typename Account::serialised_info_type GetSerialisedAccountSyncInfo(
    std::mutex& mutex,
    const AccountSet& accounts,
    const typename Account::Name& account_name);

// To be moved to Routing
bool operator ==(const routing::GroupSource& lhs,  const routing::GroupSource& rhs);

/* Commented by Mahmoud on 2 Sep -- It may be of no use any more
// Returns true if the required successful request count has been reached
template<typename Accumulator>
bool AddResult(const nfs::Message& message,
               const routing::ReplyFunctor& reply_functor,
               const maidsafe_error& return_code,
               Accumulator& accumulator,
               std::mutex& accumulator_mutex,
               int requests_required);
*/

}  // namespace detail

template <typename ServiceHandler,
          typename MessageType,
          typename AccumulatorVariantType>
struct OperationHandlerWrapper {
  typedef detail::OperationHandler<
              typename detail::ValidateSenderType<MessageType>::type,
              Accumulator<AccumulatorVariantType>,
              typename Accumulator<AccumulatorVariantType>::AddCheckerFunctor,
              ServiceHandler> TypedOperationHandler;

  OperationHandlerWrapper(Accumulator<AccumulatorVariantType>& accumulator,
                          typename detail::ValidateSenderType<MessageType>::type validate_sender,
                          typename Accumulator<AccumulatorVariantType>::AddCheckerFunctor checker,
                          ServiceHandler* service,
                          std::mutex& mutex)
      : typed_operation_handler(validate_sender, accumulator, checker, service, mutex) {}

  void operator() (const MessageType& message,
                   const typename MessageType::Sender& sender,
                   const typename MessageType::Receiver& receiver) {
    typed_operation_handler(message, sender, receiver);
  }

 private:
  TypedOperationHandler typed_operation_handler;
};

template <typename T>
struct RequiredRequests {
  static const int value = detail::RequiredValue<T::Sender>::value;
};


//template<typename Message>
//inline bool FromMaidManager(const Message& message);
//
//template<typename Message>
//inline bool FromDataManager(const Message& message);
//
//template<typename Message>
//inline bool FromPmidManager(const Message& message);
//
//template<typename Message>
//inline bool FromDataHolder(const Message& message);
//
//template<typename Message>
//inline bool FromClientMaid(const Message& message);
//
//template<typename Message>
//inline bool FromClientMpid(const Message& message);
//
//template<typename Message>
//inline bool FromOwnerDirectoryManager(const Message& message);
//
//template<typename Message>
//inline bool FromGroupDirectoryManager(const Message& message);
//
//template<typename Message>
//inline bool FromWorldDirectoryManager(const Message& message);
//
//template<typename Message>
//inline bool FromDataGetter(const Message& message);
//
//template<typename Message>
//inline bool FromVersionManager(const nfs::Message& message);

/* Commented by Mahmoud on 2 Sep -- It may be of no use any more
template<typename Persona>
typename Persona::DbKey GetKeyFromMessage(const nfs::Message& message) {
  if (!message.data().type)
    ThrowError(CommonErrors::parsing_error);
  return GetDataNameVariant(*message.data().type, message.data().name);
}
*/
std::unique_ptr<leveldb::DB> InitialiseLevelDb(const boost::filesystem::path& db_path);

}  // namespace vault

}  // namespace maidsafe

#include "maidsafe/vault/utils-inl.h"

#endif  // MAIDSAFE_VAULT_UTILS_H_
