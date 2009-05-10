#include "server/protobuf_connection.hpp"
// Encoder the Protobuf to line format.
// The line format is:
// length:content
typedef pair<const string *, const string *> EncodeData;
inline EncodeData EncodeMessage(const google::protobuf::Message *msg) {
  string *content = new string;
  if (!msg->AppendToString(content)) {
    delete content;
    return make_pair(static_cast<const string *>(NULL),
                     static_cast<const string *>(NULL));
  }
  string *header = new string(boost::lexical_cast<string>(content->size()));
  header->push_back(':');
  VLOG(2) << "Encode Message, header: " << *header << " content: " << *content
          << " content size: " << content->size();
  return make_pair(header, content);
};

template <>
template <>
void ConnectionImpl<ProtobufDecoder>::InternalPushData<EncodeData>(
    const EncodeData &data) {
  if (data.first == NULL) {
    LOG(WARNING) << "Push NULL data!";
    return;
  }
  if (duplex_[incoming_index_] == NULL) {
    duplex_[incoming_index_].reset(new vector<SharedConstBuffer>);
  }
  vector<SharedConstBuffer> *writer = duplex_[incoming_index_].get();
  writer->push_back(SharedConstBuffer(data.first));
  writer->push_back(SharedConstBuffer(data.second));
}

boost::tribool ProtobufDecoder::Consume(char input) {
  switch (state_) {
    case End:
    case Start:
      {
        if (!isdigit(input)) {
          return false;
        }
        state_ = Length;
        length_store_.clear();
        length_store_.push_back(input);
        return boost::indeterminate;
      }
    case Length:
      if (input == ':') {
        state_ = Content;
        length_ = boost::lexical_cast<int>(length_store_);
        content_.reserve(length_);
        return boost::indeterminate;
      } else if (!isdigit(input)) {
        return false;
      } else {
        length_store_.push_back(input);
        return boost::indeterminate;
      }
    case Content:
      if (content_.full()) {
        return false;
      }
      content_.push_back(input);
      if (content_.full()) {
        if (!meta_.ParseFromArray(content_.content(),
                                  content_.capacity())) {
          LOG(WARNING) << "Parse content error";
          return false;
        }
        if (meta_.type() == ProtobufLineFormat::MetaData::REQUEST &&
            !meta_.has_response_identify()) {
          LOG(WARNING) << "request meta data should have response identify field";
          return false;
        }
        VLOG(2) << "Meta: " << meta_.DebugString();
        if (meta_.content().empty()) {
          LOG(WARNING) << "Meta without content: " << meta_.DebugString();
          return false;
        }
        state_ = End;
        return true;
      }
      return boost::indeterminate;
    default:
      return false;
  }
}

static void CallServiceMethodDone(
    ProtobufConnection *connection,
    shared_ptr<const ProtobufDecoder> decoder,
    shared_ptr<google::protobuf::Message> resource,
    shared_ptr<google::protobuf::Message> response) {
  const ProtobufLineFormat::MetaData &request_meta = decoder->meta();
  ProtobufLineFormat::MetaData response_meta;
  response_meta.set_type(ProtobufLineFormat::MetaData::RESPONSE);
  response_meta.set_identify(request_meta.response_identify());
  CHECK(response->AppendToString(response_meta.mutable_content()))
    << "Fail to serialize response for requst: " << request_meta.DebugString();
  connection->PushData(EncodeMessage(&response_meta));
  connection->ScheduleWrite();
  VLOG(3) << "CallServiceMethodDone()";
}

static void HandlerService(
    google::protobuf::Service *service,
    const google::protobuf::MethodDescriptor *method,
    const google::protobuf::Message *request_prototype,
    const google::protobuf::Message *response_prototype,
    shared_ptr<const ProtobufDecoder> decoder,
    ProtobufConnection *connection) {
  VLOG(2) << "Handler service: " << method->full_name();
  const ProtobufLineFormat::MetaData &meta = decoder->meta();
  shared_ptr<google::protobuf::Message> request(request_prototype->New());
  const string &content = decoder->meta().content();
  VLOG(2) << "content size: " << content.size();
  if (!request->ParseFromArray(
      content.c_str(),
      content.size())) {
    LOG(WARNING) << meta.DebugString() << " invalid format";
    return;
  }
  shared_ptr<google::protobuf::Message> response(response_prototype->New());
  google::protobuf::Closure *done = NewClosure(
      boost::bind(CallServiceMethodDone,
                  connection,
                  decoder,
                  request,
                  response));
  service->CallMethod(method, connection, request.get(), response.get(), done);
}

bool ProtobufConnection::RegisterService(google::protobuf::Service *service) {
  if (this->IsConnected()) {
    LOG(WARNING) << "Can't register service to running connection.";
    return false;
  }
  const google::protobuf::ServiceDescriptor *service_descriptor =
    service->GetDescriptor();
  for (int i = 0; i < service_descriptor->method_count(); ++i) {
    const google::protobuf::MethodDescriptor *method = service_descriptor->method(i);
    const google::protobuf::Message *request = &service->GetRequestPrototype(method);
    const google::protobuf::Message *response = &service->GetResponsePrototype(method);
    const string &method_name = method->full_name();
    const uint64 method_fingerprint = hash8(method_name);
    HandlerTable::const_iterator it = handler_table_->find(method_fingerprint);
    CHECK(it == handler_table_->end())
      << " unfortunately, the method name: " << method_name
      << " is conflict with another name after hash("
      << method_fingerprint << ") please change.";
    handler_table_->insert(make_pair(
        method_fingerprint,
        boost::bind(
        HandlerService, service, method, request, response,
        _1, _2)));
  }
}

void ProtobufConnection::Handle(shared_ptr<const ProtobufDecoder> decoder) {
  const ProtobufLineFormat::MetaData &meta = decoder->meta();
  VLOG(2) << "Handle request: " << meta.DebugString();
  HandlerTable::iterator it = handler_table_->find(meta.identify());
  if (it != handler_table_->end()) {
    it->second(decoder, this);
    return;
  }
  HandlerTable::value_type::second_type handler;
  {
    boost::mutex::scoped_lock locker(response_handler_table_mutex_);
    it = response_handler_table_.find(meta.identify());
    if (it == response_handler_table_.end()) {
      VLOG(2) << "Unknown request" << meta.DebugString();
      return;
    }
    handler = it->second;
    response_handler_table_.erase(it);
  }
  handler(decoder, this);
}

void ProtobufConnection::CallMethod(const google::protobuf::MethodDescriptor *method,
                  google::protobuf::RpcController *controller,
                  const google::protobuf::Message *request,
                  google::protobuf::Message *response,
                  google::protobuf::Closure *done) {
  uint64 request_identify = hash8(method->full_name());
  uint64 response_identify = hash8(response->GetDescriptor()->full_name());
  boost::mutex::scoped_lock locker(response_handler_table_mutex_);
  HandlerTable::const_iterator it = response_handler_table_.find(response_identify);
  while (it != response_handler_table_.end()) {
    it = response_handler_table_.find(++response_identify);
  }
  ProtobufLineFormat::MetaData meta;
  meta.set_identify(request_identify);
  meta.set_type(ProtobufLineFormat::MetaData::REQUEST);
  meta.set_response_identify(response_identify);
  if (!request->AppendToString(meta.mutable_content())) {
    LOG(WARNING) << "Fail to serialze request form method: "
                 << method->full_name();
  }
  response_handler_table_.insert(make_pair(
      response_identify,
      boost::bind(&ProtobufConnection::CallMethodCallback, this,
                  _1, _2, controller, response, done)));
  PushData(EncodeMessage(&meta));
  ScheduleWrite();
  ScheduleRead();
}

void ProtobufConnection::CallMethodCallback(
    shared_ptr<const ProtobufDecoder> decoder,
    ProtobufConnection *,
    google::protobuf::RpcController *controller,
    google::protobuf::Message *response,
    google::protobuf::Closure *done) {
  const ProtobufLineFormat::MetaData &meta = decoder->meta();
  VLOG(3) << "Handle response message "
          << response->GetDescriptor()->full_name()
          << " response: " << meta.DebugString();
  if (!response->ParseFromArray(meta.content().c_str(),
                                meta.content().size())) {
    LOG(WARNING) << "Fail to parse the response :"
                 << meta.DebugString();
    controller->SetFailed("Fail to parse the response:" + meta.DebugString());
    if (done) done->Run();
    return;
  }
  if (done) done->Run();
}
