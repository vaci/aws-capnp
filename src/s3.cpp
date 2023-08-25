// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "s3.h"

#include "common.h"
#include "http.h"

#include "capnp/compat/byte-stream.h"

#include <kj/io.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <rapidxml/rapidxml.hpp>
#include <iostream>

namespace aws {

namespace {

struct S3Server;
struct BucketServer;
struct ObjectServer;

capnp::ByteStream::Client multipart(
  kj::Own<ObjectServer> object,
  kj::StringPtr key,
  kj::StringPtr contentType,
  capnp::List<capnp::HttpHeader>::Reader headers);

  
struct ObjectServer
  : S3::Object::Server
  , kj::TaskSet::ErrorHandler {

  ObjectServer(
      kj::Own<BucketServer> bucket,
      kj::StringPtr key)
    : tasks_{*this}
    , bucket_{kj::mv(bucket)}
    , key_{kj::str(key)} {
  }

  void taskFailed(kj::Exception&& exc) override {
    KJ_LOG(ERROR, exc);
  }

  kj::Promise<void> head(HeadContext) override;
  kj::Promise<void> getBucket(GetBucketContext) override;
  kj::Promise<void> read(ReadContext) override;
  kj::Promise<void> write(WriteContext) override;
  kj::Promise<void> versions(VersionsContext) override;

  kj::TaskSet tasks_;
  kj::Own<BucketServer> bucket_;
  kj::String key_;
};

struct BucketServer
  : S3::Bucket::Server
  , kj::Refcounted {

  BucketServer(kj::Own<S3Server>, kj::StringPtr name, kj::StringPtr region);

  kj::Own<BucketServer> addRef() {
    return kj::addRef(*this);
  }

  kj::Promise<void> getObject(GetObjectContext) override;

  kj::Own<S3Server> s3_;
  kj::String name_;
  kj::String hostname_;
  kj::HttpHeaders headers_;
};

struct S3Server
  : S3::Server
  , kj::Refcounted
  , kj::TaskSet::ErrorHandler {

  S3Server(
    kj::HttpHeaderTable::Builder& builder,
    kj::Own<kj::HttpClient> awsService,
    kj::StringPtr region,
    kj::Maybe<capnp::ByteStreamFactory&> = nullptr
  );

  kj::Own<S3Server> addRef() {
    return kj::addRef(*this);
  }
  
  void taskFailed(kj::Exception&& exc) override {
    KJ_LOG(ERROR, exc);
  }

  kj::Promise<void> list(ListContext) override;
  kj::Promise<void> getBucket(GetBucketContext) override;

  struct {
    kj::HttpHeaderId etag;
    kj::HttpHeaderId range;
  } ids_;

  
  kj::TaskSet tasks_;
  kj::Own<kj::HttpClient> awsService_;
  kj::HttpHeaderTable& table_;
  kj::StringPtr region_;
  kj::String hostname_;

  kj::Own<capnp::ByteStreamFactory> factory_;
};

S3Server::S3Server(
  kj::HttpHeaderTable::Builder& builder,
  kj::Own<kj::HttpClient> awsService,
  kj::StringPtr region,
  kj::Maybe<capnp::ByteStreamFactory&> factory)
  : tasks_{*this}
  , awsService_{kj::mv(awsService)}
  , ids_{ 
      .etag{builder.add("etag")},
      .range{builder.add("range")}
  }
  , table_{builder.getFutureTable()}
  , region_{region}
  , hostname_{kj::str("s3."_kj, region_, ".amazonaws.com")} {

  KJ_IF_MAYBE(f, factory) {
    factory_ = kj::Own<capnp::ByteStreamFactory>(f, kj::NullDisposer::instance);
  }
}

kj::Promise<void> S3Server::list(ListContext ctx) {

  KJ_DREQUIRE(headerTable_.isReady());

  auto url = kj::str("https://"_kj, hostname_, "/"_kj);

  kj::HttpHeaders headers{table_};
  headers.set(kj::HttpHeaderId::HOST, hostname_);
  auto req = awsService_->request(kj::HttpMethod::GET, url, headers);

  auto params = ctx.getParams();
  auto callback = params.getCallback();

  return
    req.response
    .then(
      [this](auto response) mutable {
	return response.body->readAllText().attach(kj::mv(response.body));
      }
    )
    .then(
      [callback](auto txt) mutable {

	kj::Vector<kj::Promise<void>> promises;

	rapidxml::xml_document<> doc;
	doc.parse<rapidxml::parse_non_destructive>(const_cast<char*>(txt.cStr()));

	auto first_node = [](auto node, auto name) {
	  return node->first_node(name.begin(), name.size());
	};

	auto next_sibling = [](auto node, auto name) {
	  return node->next_sibling(name.begin(), name.size());
	};

	auto result = first_node(&doc, "ListAllMyBucketsResult"_kj);
	result = first_node(result, "Buckets"_kj);
	result = first_node(result, "Bucket"_kj);

	while (result) {
	  auto name = first_node(result, "Name"_kj);
	  auto value = kj::StringPtr{name->value(), name->value_size()};
	  auto req = callback.nextRequest();
	  req.setValue(value);
	  promises.add(req.send().ignoreResult());
	  result = next_sibling(result, "Bucket"_kj);
	}

	return kj::joinPromises(promises.releaseAsArray());
      }
    )
    .then(
      [callback]() mutable {
	auto req = callback.endRequest();
	return req.send().ignoreResult();
      }
    );
}

kj::Promise<void> S3Server::getBucket(GetBucketContext ctx) {
  auto params = ctx.getParams();
  auto name = params.getName();
  auto reply = ctx.getResults();
  reply.setBucket(kj::refcounted<BucketServer>(addRef(), name, region_));
  return kj::READY_NOW;
}
  
BucketServer::BucketServer(kj::Own<S3Server> s3, kj::StringPtr name, kj::StringPtr region)
  : s3_{kj::mv(s3)}
  , name_{kj::str(name)}
  , hostname_{kj::str(name_, ".s3.", region, ".amazonaws.com")}
  , headers_{s3_->table_} {

  headers_.set(kj::HttpHeaderId::HOST, hostname_);
}

kj::Promise<void> BucketServer::getObject(GetObjectContext ctx) {
  auto params = ctx.getParams();
  auto key = params.getKey();
  auto reply = ctx.getResults();
  reply.setObject(kj::heap<ObjectServer>(addRef(), key));
  return kj::READY_NOW;
}

kj::Promise<void> ObjectServer::head(HeadContext ctx) {
  auto url = kj::str("https://"_kj, bucket_->hostname_, "/"_kj, key_);

  auto req = bucket_->s3_->awsService_->request(
    kj::HttpMethod::HEAD, url, bucket_->headers_, 0ul
  );

  return
    req.response
    .then(
      [this, ctx = kj::mv(ctx)](auto response) mutable {
	auto reply = ctx.getResults();
	reply.setKey(key_);
	KJ_LOG(INFO, response.statusText);

	auto headers = reply.initHeaders(response.headers->size());
	auto ii = 0u;
	response.headers->forEach(
          [&](auto name, auto value) {
	    auto header = headers[ii++].initUncommon();
	    header.setName(name);
	    header.setValue(value);
	    KJ_LOG(INFO, name, value);
	  }
	);
      }
    );
}

kj::Promise<void> ObjectServer::getBucket(GetBucketContext ctx) {
  auto reply = ctx.getResults();
  reply.setBucket(bucket_->addRef());
  return kj::READY_NOW;
}

kj::Promise<void> ObjectServer::read(ReadContext ctx) {
  auto params = ctx.getParams();
  auto stream = params.getStream();
  auto first = params.getFirst();
  auto last = params.getLast();

  auto out = bucket_->s3_->factory_->capnpToKj(kj::mv(stream));

  auto headers = bucket_->headers_.cloneShallow();
  headers.set(bucket_->s3_->ids_.range, kj::str(first, '-', last));

  auto req = bucket_->s3_->awsService_->request(
    kj::HttpMethod::GET,
    kj::str("https://", bucket_->hostname_, "/", key_),
    headers, 0ul
  );

  return
    req.response
    .then(
      [this, ctx = kj::mv(ctx), out = kj::mv(out)](auto response) mutable {
	auto& headers = response.headers;
	
	headers->forEach(
	  [](auto name, auto value) {
	    KJ_LOG(INFO, name, value);
	  }
	);

	auto length = [&]{
	  auto maybeValue = headers->get(kj::HttpHeaderId::CONTENT_LENGTH);
	  auto value = KJ_REQUIRE_NONNULL(maybeValue);
	  return value.template parseAs<uint64_t>();
	}();
	  
	auto reply = ctx.getResults();
	reply.setLength(length);

	tasks_.add(
	  response.body->pumpTo(*out)
	  .ignoreResult()
	  .attach(kj::mv(response.body), kj::mv(out))
	);
      }
    );
}

kj::Promise<void> ObjectServer::write(WriteContext ctx) {
  return kj::READY_NOW;
}

kj::Promise<void> ObjectServer::versions(VersionsContext ctx) {
  return kj::READY_NOW;
}

struct MultipartStream
  : capnp::ByteStream::Server
  , kj::AsyncOutputStream
  , kj::TaskSet::ErrorHandler {

  MultipartStream(
    kj::Own<ObjectServer> object,
    kj::StringPtr uploadId);

  kj::Promise<void> write(WriteContext ctx) override {
    auto params = ctx.getParams();
    auto bytes = params.getBytes();
    return write(bytes.begin(), bytes.size());
  }

  kj::Promise<void> end(EndContext) override {
    return finish().ignoreResult();
  }

  kj::Promise<void> write(void const* data, size_t size) override {
    return write(kj::arrayPtr(reinterpret_cast<kj::byte const*>(data), size));
  }

  kj::Promise<void> write(
      kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    while (pieces.size() > 0 && pieces[0].size() == 0) {
      pieces = pieces.slice(1, pieces.size());
    }

    if (pieces.size() == 0) {
      return kj::READY_NOW;
    }

    return
      write(pieces.front())
      .then(
        [this, pieces] {
	  return write(pieces.slice(1, pieces.size()));
	}
      );
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

private:
    void taskFailed(kj::Exception&& exc) override {
    KJ_LOG(ERROR, exc);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte>);
  kj::Promise<void> sendPart(kj::ArrayPtr<kj::byte const>);
  kj::Promise<kj::String> complete();
  kj::Promise<kj::String> finish();

  kj::TaskSet tasks_{*this};
  kj::Own<ObjectServer> object_;
  kj::String uploadId_;
  std::size_t bufferSize_;

  kj::Array<kj::byte> buffer_;

  struct Part {
    uint32_t partNumber_;
    kj::String etag_;
  };

  kj::Vector<Part> parts_;
  kj::Own<kj::ArrayOutputStream> out_;

};

MultipartStream::MultipartStream(
    kj::Own<ObjectServer> object,
    kj::StringPtr uploadId)
  : object_{kj::mv(object)}
  , uploadId_{kj::str(uploadId)} {
}

kj::Promise<void> MultipartStream::write(kj::ArrayPtr<kj::byte const> bytes) {
  if (bytes.size() == 0) {
    return kj::READY_NOW;
  }

  auto remaining = buffer_.size() - out_->getArray().size();
  KJ_DREQUIRE(remaining);

  if (bytes.size() <= remaining) {
    out_->write(bytes.begin(), bytes.size());

    if (bytes.size() == remaining) {
      auto data = out_->getArray();
      tasks_.add(sendPart(buffer_.slice(0, data.size())).attach(kj::mv(buffer_)));
      buffer_ = kj::heapArray<kj::byte>(bufferSize_);
      out_ = kj::heap<kj::ArrayOutputStream>(buffer_);
    }

    return kj::READY_NOW;
  }
  else {
    return write(bytes.slice(0, remaining))
      .then(
          [this, remaining, bytes]{
            return write(bytes.slice(remaining, bytes.size()));
          }
      );
  }
}

kj::Promise<void> MultipartStream::sendPart(
    kj::ArrayPtr<const kj::byte> buffer) {

  auto& part = parts_.add();
  auto partNumber = parts_.size();
  part.partNumber_ = partNumber;

  auto url = kj::str(
    "https://", object_->bucket_->hostname_, "/", object_->key_,
    "?partNumber=", partNumber,
    "&uploadId=", uploadId_
  );
		
  auto headers = object_->bucket_->headers_.cloneShallow();

  auto req = object_->bucket_->s3_->awsService_->request(
    kj::HttpMethod::PUT, url, headers, buffer.size()
  );

  return
    req.body->write(buffer.begin(), buffer.size())
    .then(
      [req = kj::mv(req)]() mutable {
	req.body = nullptr;
	return kj::mv(req.response);
      }
    )
    .then(
      [this, partNumber](auto response) {
	auto& headers = response.headers;
	auto etag = KJ_REQUIRE_NONNULL(headers->get(object_->bucket_->s3_->ids_.etag));
	parts_[partNumber-1].etag_ = kj::str(etag);
      }
    );
} 

kj::Promise<kj::String> MultipartStream::complete() {
  KJ_LOG(INFO, "Completing", uploadId_);
  KJ_DREQUIRE(started_);

  auto txt = kj::strTree(
    "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"_kj,
    KJ_MAP(part, parts_) {
      return kj::strTree(
        "<Part><PartNumber>"_kj,
	part.partNumber_,
	"</PartNumber><ETag>"_kj,
	part.etag_,
	"</ETag></Part>"_kj
      );
    },
    "</CompleteMultipartUpload>"_kj
  ).flatten();
    
  auto url = kj::str(
    "https://", object_->bucket_->hostname_, "/", object_->key_,
    "&uploadId=", uploadId_
  );

  auto headers = object_->bucket_->headers_.cloneShallow();		     
  auto req = object_->bucket_->s3_->awsService_->request(
    kj::HttpMethod::POST, url, headers, txt.size()
  );

  return
    req.body->write(txt.begin(), txt.size())
    .then(
      [req = kj::mv(req)]() mutable {
        req.body = nullptr;
	return kj::mv(req.response);
      }
    )
    .then(
      [this](auto response) {
	auto& headers = response.headers;
	auto etag = KJ_REQUIRE_NONNULL(headers->get(object_->bucket_->s3_->ids_.etag));
	return kj::str(etag);
      }
    );
}

kj::Promise<kj::String> MultipartStream::finish() {
  return
    tasks_.onEmpty()
    .then(
        [this]{
          // send any remaining partial data
          auto data = out_->getArray();
          return data.size()
            ? sendPart(buffer_.slice(0, data.size()))
            : kj::READY_NOW;
        }
    )
    .then(
        [this]{
          return complete();
        }
    );
}

  /*
    <InitiateMultipartUploadResult>
   <Bucket>string</Bucket>
   <Key>string</Key>
   <UploadId>string</UploadId>
</InitiateMultipartUploadResult>
  */

capnp::ByteStream::Client multipart(
  kj::Own<ObjectServer> object,
  kj::StringPtr key,
  kj::StringPtr contentType) {

  auto& bucket = object->bucket_;

  auto url = kj::str(
    "https://", bucket->hostname_, "/", object->key_, "?uploads"_kj
  );
  auto headers = bucket->headers_.cloneShallow();		     
  auto req = bucket->s3_->awsService_->request(
    kj::HttpMethod::POST, url, headers, 0ul
  );
  return
    req.response
    .then(
      [object = kj::mv(object)](auto response) mutable {
	return response.body->readAllBytes();
      }
    )
    .then(
      [object = kj::mv(object)](auto bytes) mutable {


	auto stream = kj::heap<MultipartStream>(kj::mv(object), kj::str());
	return capnp::ByteStream::Client{kj::mv(stream)};
      }
    );
}

}

aws::S3::Client newS3(
  kj::HttpHeaderTable::Builder& builder,
  kj::Own<kj::HttpClient> awsService,
  kj::StringPtr region) {
  return kj::refcounted<S3Server>(builder, kj::mv(awsService), region);
}

}
