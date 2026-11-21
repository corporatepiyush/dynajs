/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:protobuf
/** dyna:protobuf — protobuf wire codec over a { fields } schema; decode bounded (nesting <= 64). */
declare module "dyna:protobuf" {
    /** Protobuf wire encoding/decoding driven by a plain-object descriptor. */
    interface ProtoField {
        name: string;
        number: number;
        type: string;
        repeated?: boolean;
        packed?: boolean;
        keyType?: string;
        valueType?: string;
        message?: { fields: ProtoField[] };
    }
    /** Protobuf wire codec driven by a { fields } schema object (no .proto compiler). */
    const Proto: {
        /** Wire encoding, strict about JS types and ranges; `oneof` is an ANNOTATION only (last one wins, as in proto2/3). */
        encode(value: unknown, schema: { fields: ProtoField[] }): Uint8Array;
        /** The untrusted surface: lengths validated, nesting capped at 64. */
        decode(bytes: ByteView, schema: { fields: ProtoField[] }): unknown;
    };
}
