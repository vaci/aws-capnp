// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "s3.h"

#include <capnp/compat/byte-stream.h>

#include <kj/compat/http.h>
#include <kj/compat/url.h>
#include <kj/encoding.h>
#include <kj/filesystem.h>

namespace aws {

namespace {

struct HttpService
  : kj::HttpService {

  HttpService(kj::HttpHeaderTable::Builder&, S3::Client);

  kj::Promise<void> request(
    kj::HttpMethod, kj::StringPtr url, const kj::HttpHeaders&,
    kj::AsyncInputStream&, Response&) override;

  struct {
    kj::HttpHeaderId accept;
    kj::HttpHeaderId amzSdkInvocationId;
    kj::HttpHeaderId amzSdkRequest;
    kj::HttpHeaderId auth;
    kj::HttpHeaderId xAmzContentSha256;
    kj::HttpHeaderId xAmzDate;
    kj::HttpHeaderId xAmzSecurityToken;
  } ids_;

  kj::HttpHeaderTable& table_;
  S3::Client s3_;
};

struct S3ServerImpl
  : S3::Server
  , kj::Refcounted
  , kj::TaskSet::ErrorHandler {

  S3ServerImpl(
    kj::Own<const kj::Directory>,
    capnp::ByteStreamFactory&
  );

  kj::Own<S3ServerImpl> addRef() {
    return kj::addRef(*this);
  }

  void taskFailed(kj::Exception&& exc) override {
    KJ_LOG(INFO, exc);
  }

  kj::Promise<void> listBuckets(ListBucketsContext) override;
  kj::Promise<void> getBucket(GetBucketContext) override;
  kj::Promise<void> createBucket(CreateBucketContext) override;

  kj::Own<const kj::Directory> dir_;
  capnp::ByteStreamFactory& factory_;
  kj::TaskSet tasks_{*this};
};

struct BucketServerImpl
  : S3::Bucket::Server
  , kj::Refcounted {

  BucketServerImpl(kj::Own<S3ServerImpl> s3, kj::StringPtr name)
    : s3_{kj::mv(s3)}
    , name_{kj::str(name)}
    , hex_{kj::encodeHex(name_.asBytes())} {
  }

  kj::Own<BucketServerImpl> addRef() {
    return kj::addRef(*this);
  }

  kj::Promise<void> listObjects(ListObjectsContext) override;
  kj::Promise<void> listObjectVersions(ListObjectVersionsContext) override;
  kj::Promise<void> getObject(GetObjectContext) override;
  kj::Own<S3ServerImpl> s3_;
  kj::String name_;
  kj::String hex_;
};

struct ObjectServerImpl
  : S3::Object::Server
  , kj::Refcounted{

  ObjectServerImpl(kj::Own<BucketServerImpl> bucket, kj::StringPtr key)
    : bucket_{kj::mv(bucket)}
    , key_{kj::str(key)}
    , hex_{kj::encodeHex(key_.asBytes())} {
  }

  kj::Promise<void> read(ReadContext) override;
  kj::Promise<void> write(WriteContext) override;

  kj::Own<BucketServerImpl> bucket_;
  kj::String key_;
  kj::String hex_;
};


HttpService::HttpService(kj::HttpHeaderTable::Builder& builder, S3::Client s3)
  : ids_{
      .accept{builder.add("accept")},
      .amzSdkInvocationId{builder.add("amz-sdk-invocation-id")},
      .amzSdkRequest{builder.add("amz-sdk-request")},
      .auth{builder.add("authorization")},
      .xAmzContentSha256{builder.add("x-amz-content-sha256")},
      .xAmzDate{builder.add("x-amz-date")},
      .xAmzSecurityToken{builder.add("X-Amz-Security-Token")}
  },
  table_{builder.getFutureTable()},
  s3_{kj::mv(s3)} {
}

/*
  <?xml version="1.0" encoding="UTF-8"?>
<ListAllMyBucketsResult>
   <Buckets>
      <Bucket>
         <CreationDate>timestamp</CreationDate>
         <Name>string</Name>
      </Bucket>
   </Buckets>
   <Owner>
      <DisplayName>string</DisplayName>
      <ID>string</ID>
   </Owner>
</ListAllMyBucketsResult>
*/

kj::Promise<void> HttpService::request(
  kj::HttpMethod method, kj::StringPtr urlTxt, const kj::HttpHeaders& headers,
  kj::AsyncInputStream& body, Response& response) {

  auto url = kj::Url::parse(urlTxt);
  auto host = KJ_REQUIRE_NONNULL(headers.get(kj::HttpHeaderId::HOST));

  if (method == kj::HttpMethod::GET && url.path.size() == 0 && host == "s3.amazonaws.com") {
    // ListBuckets

    auto req = s3_.listBucketsRequest();
    return
      req.send().then(
	[&](auto reply) {
	  auto names = reply.getBucketNames();
	  auto txt = kj::strTree(
	    R"(<?xml version="1.0" encoding="UTF-8"?>)"_kj,
	    "<ListAllMyBucketsResult><Buckets>"_kj,
	    KJ_MAP(name, names) {
	      return kj::strTree(
		"<Bucket>"_kj,
		"<Name>"_kj, name, "</Name>"_kj,
		"<CreationDate>2019-12-11T23:32:47+00:00</CreationDate>"_kj,
		"<Bucket>"_kj
	      );
	    },
	    "</Buckets></ListAllMyBucketsResult>"_kj
	  ).flatten();

	  kj::HttpHeaders headers{table_};
	  auto body = response.send(200, "OK"_kj, headers, txt.size()); 
	  return body->write(txt.begin(), txt.size()).attach(kj::mv(body));
	}
      );
  }
  
  return kj::READY_NOW;
}

kj::Promise<void> BucketServerImpl::listObjects(ListObjectsContext ctx) {
  auto params = ctx.getParams();
  auto prefix = kj::encodeHex(params.getPrefix().asBytes());
  auto callback = params.getCallback();

  kj::Vector<kj::Promise<void>> matches;

  auto dir = s3_->dir_->openSubdir(kj::Path{hex_});
  for (auto&& hex: dir->listNames()) {
    if (!hex.startsWith(prefix)) {
      continue;
    }

    auto req = callback.nextRequest();
    req.setValue(kj::str(kj::decodeHex(hex)));
    matches.add(req.send().ignoreResult());
  }
  return kj::joinPromises(matches.releaseAsArray());
}

kj::Promise<void> BucketServerImpl::listObjectVersions(ListObjectVersionsContext ctx) {
  auto params = ctx.getParams();
  auto prefix = kj::encodeHex(params.getPrefix().asBytes());
  auto callback = params.getCallback();

  kj::Vector<kj::Promise<void>> matches;

  auto dir = s3_->dir_->openSubdir(kj::Path{hex_});

  for (auto&& hex: dir->listNames()) {
    if (!hex.startsWith(prefix)) {
      continue;
    }

    auto objDir = dir->openSubdir(kj::Path{hex, "versions"});
    auto key = kj::str(kj::decodeHex(hex));
    
    for (auto&& version: objDir->listNames()) {
      auto req = callback.nextRequest();
      {
	auto value = req.initValue();
	value.setKey(key);
	value.setVersion(version);
	value.setDeleted(false);
      }
      matches.add(req.send().ignoreResult());
    }
  }
  return kj::joinPromises(matches.releaseAsArray());
}

S3ServerImpl::S3ServerImpl(
  kj::Own<const kj::Directory> dir,
  capnp::ByteStreamFactory& factory)
  : dir_{kj::mv(dir)}
  , factory_{factory} {
}

kj::Promise<void> S3ServerImpl::listBuckets(ListBucketsContext ctx) {
  auto params = ctx.getParams();
  auto names = dir_->listNames();
  auto reply = ctx.getResults();
  {
    auto builder = reply.initBucketNames(names.size());
    for (auto ii: kj::indices(names)) {
      builder.set(ii, names[ii]);
    }
  }
  return kj::READY_NOW;
}

kj::Promise<void> BucketServerImpl::getObject(GetObjectContext ctx) {
  auto params = ctx.getParams();
  auto key = params.getKey();
  auto reply = ctx.getResults();
  reply.setObject(kj::refcounted<ObjectServerImpl>(addRef(), key));
  return kj::READY_NOW;
}

kj::Promise<void> S3ServerImpl::createBucket(CreateBucketContext ctx) {
  auto params = ctx.getParams();
  auto name = params.getName();

  auto bucket = kj::refcounted<BucketServerImpl>(addRef(), name);
  dir_->openSubdir(kj::Path{bucket->hex_}, kj::WriteMode::CREATE);

  auto reply = ctx.getResults();
  reply.setBucket(kj::mv(bucket));
  return kj::READY_NOW;
}

kj::Promise<void> ObjectServerImpl::read(ReadContext ctx) {
  auto params = ctx.getParams();
  auto path = kj::Path{bucket_->hex_, hex_, "versions"};
  auto dir = bucket_->s3_->dir_->openSubdir(path);

  auto version = kj::str(params.getVersion());
  if (!version.size()) {
    auto names = dir->listNames();
    KJ_REQUIRE(names.size());
    version = kj::str(dir->listNames().back());
  }

  auto file = dir->openFile(kj::Path::parse(version));
  auto stat = file->stat();
  auto data = file->mmap(0, stat.size);
  auto stream = bucket_->s3_->factory_.capnpToKj(params.getStream());
  return
    stream->write(data.begin(), data.size())
    .attach(kj::mv(stream), kj::mv(data));
}

kj::Promise<void> writeImpl(
  kj::Own<kj::AsyncInputStream> input,
  kj::Own<kj::OutputStream> output,
  kj::Array<kj::byte> buffer = kj::heapArray<kj::byte>(65536u)) {

  return
    input->tryRead(buffer.begin(), 1, buffer.size())
    .then(
      [
	input = kj::mv(input),
	output = kj::mv(output),
	buffer = kj::mv(buffer)
      ](auto size) mutable -> kj::Promise<void> {
	if (size) {
	  output->write(buffer.begin(), size);
	  return writeImpl(kj::mv(input), kj::mv(output), kj::mv(buffer));
	}
	else {
	  // EOF
	  return kj::READY_NOW;
	}
      }
    );
}

kj::Promise<void> ObjectServerImpl::write(WriteContext ctx) {
  auto params = ctx.getParams();
  auto length = params.getLength();
  auto path = kj::Path{bucket_->hex_, hex_, "versions"};
  auto dir = bucket_->s3_->dir_->openSubdir(
    path, kj::WriteMode::CREATE|kj::WriteMode::MODIFY|kj::WriteMode::CREATE_PARENT
  );

  auto version = [&]{
    auto names = dir->listNames();
    if (!names.size()) {
      return 0u;
    }
    else {
      auto version = names.back().parseAs<uint32_t>();
      return version+1;
    }
  }();

  auto file = dir->appendFile(kj::Path{kj::str(version)}, kj:: WriteMode::CREATE);

  auto pipe = kj::newOneWayPipe();

  auto stream = bucket_->s3_->factory_.kjToCapnp(kj::mv(pipe.out));
  auto reply = ctx.getResults();
  reply.setStream(kj::mv(stream));

  bucket_->s3_->tasks_.add(writeImpl(kj::mv(pipe.in), kj::mv(file)));
  return kj::READY_NOW;
}

kj::Promise<void> S3ServerImpl::getBucket(GetBucketContext ctx) {
  auto params = ctx.getParams();
  auto name = params.getName();
  auto reply = ctx.getResults();
  reply.setBucket(
    kj::refcounted<BucketServerImpl>(addRef(), name)
  );  
  return kj::READY_NOW;
}

}

aws::S3::Client newS3Server(
  kj::Own<const kj::Directory> dir,
  kj::Maybe<capnp::ByteStreamFactory&> factory) {

  KJ_IF_MAYBE(f, factory) {
    return kj::refcounted<S3ServerImpl>(kj::mv(dir), *f);
  }
  else {
    auto factory = kj::heap<capnp::ByteStreamFactory>();
    return kj::refcounted<S3ServerImpl>(kj::mv(dir), *factory).attach(kj::mv(factory));
  }
}

}
