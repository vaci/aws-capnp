# Copyright (c) 2023 Vaci Koblizek.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xa4c0eb0daffa60b4;

$import "/capnp/c++.capnp".namespace("aws");

using ByteStream =  import "/capnp/compat/byte-stream.capnp".ByteStream;
using HttpHeader =  import "/capnp/compat/http-over-capnp.capnp".HttpHeader;

struct Credentials {
  accessKey @0 :Text;
  secretKey @1 :Text;
  sessionToken @2 :Text;

  interface Provider {	
    getCredentials @0 () -> Credentials;
  }
}

interface Callback(T) {
  next @0 (value :T);
  end @1 ();
}

interface S3 {
  list @0 (callback :Callback(Text));
  getBucket @1 (name :Text) -> (bucket :Bucket);

  interface Bucket {
    struct Properties {
      name @0 :Text;
    }

    head @0 () -> Properties;
    list @1 (prefix :Text = "", callback: Callback(Text));
    getObject @2 (key :Text) -> (object :Object);
  }

  interface Object {
    struct Properties {
      key @0 :Text;
      headers @1 :List(HttpHeader);
    }
    head @0 () -> Properties;
    getBucket @1 () -> (bucket :Bucket);

    read @2 (
      stream :ByteStream,
      first :UInt64 = 0,
      last :UInt64 = 0xFFFFFFFF) -> (length :UInt64);
    write @3 () -> (stream :ByteStream);
    multipart @4 () -> (stream :ByteStream);

    versions @5 (callback: Callback(Text));
  }
}


