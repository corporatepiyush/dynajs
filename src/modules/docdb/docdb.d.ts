/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:docdb
/** dyna:docdb — embedded in-memory MongoDB-semantics store (query, update, index, aggregate, change streams) over BSON
 *  arenas; no wire protocol; one JS thread owns a Database, persist/load/large aggregates run on the pool. */
declare module "dyna:docdb" {
    /** The same class as dyna:serialize's ObjectId. */
    class ObjectId {
        /** No argument generates a fresh id; a 24-hex string, 12 bytes or an ObjectId copies one. */
        constructor(id?: string | ByteView | ObjectId);
        toHexString(): string;
        /** True for an ObjectId or hex string naming the same 12 bytes. */
        equals(other: ObjectId | string): boolean;
        /** Embedded creation time, second precision. */
        getTimestamp(): Date;
    }

    /** A value a document field can hold (dyna:serialize BSON mapping: int32-valued numbers store as int32, BigInt as int64). */
    type DocValue = string | number | bigint | boolean | null | Date | RegExp | Uint8Array | ObjectId
        | import("dyna:serialize").Timestamp | import("dyna:serialize").Decimal128 | import("dyna:serialize").Binary
        | import("dyna:serialize").MinKey | import("dyna:serialize").MaxKey | readonly DocValue[] | { [key: string]: DocValue | undefined };
    /** Any document; the default schema of an untyped Collection. */
    type Doc = { [key: string]: DocValue | undefined };
    /** A document as stored: _id is always present (an ObjectId unless the schema fixes its type) and stored first. */
    type DocWithId<T> = T extends { _id: infer I } ? T & { _id: I } : T & { _id: ObjectId };
    /** An insertable document: _id may be left out (an ObjectId is generated) unless the schema fixes a non-ObjectId _id. */
    type DocInsert<T> = T extends { _id: infer I } ? ObjectId extends I ? Omit<T, "_id"> & { _id?: I } : T : T & { _id?: ObjectId };

    type DocPrev = [never, 0, 1, 2, 3, 4];
    type DocElem<V> = V extends readonly (infer E)[] ? E : never;
    type DocIsObj<V> = V extends Date | RegExp | Uint8Array | ObjectId | ByteView | readonly unknown[] ? false : V extends object ? true : false;
    /** Every dotted path of T up to 5 levels, through arrays (element fields and numeric indexes, as MongoDB). */
    type DocPaths<T, D extends number = 5> = [D] extends [never] ? never : string extends keyof T ? string : T extends readonly (infer E)[]
        ? `${number}` | DocPaths<E, D> | `${number}.${DocPaths<E, DocPrev[D]>}`
        : DocIsObj<T> extends true ? { [K in keyof T & string]-?: K | (DocSub<NonNullable<T[K]>, DocPrev[D]> extends infer S extends string ? [S] extends [never] ? never : `${K}.${S}` : never) }[keyof T & string] : never;
    type DocSub<V, D extends number> = V extends readonly (infer E)[] ? `${number}` | DocPaths<E, D> | `${number}.${DocPaths<E, DocPrev[D]>}` : DocIsObj<V> extends true ? DocPaths<V, D> : never;
    /** The value at a dotted path; through an array it is the element value (any element may match). */
    type DocPathValue<T, P extends string> = T extends readonly (infer E)[]
        ? P extends `${number}` ? E : P extends `${number}.${infer R}` ? DocPathValue<E, R> : DocPathValue<E, P>
        : P extends `${infer K}.${infer R}` ? K extends keyof T ? DocPathValue<NonNullable<T[K]>, R> | (undefined extends T[K] ? undefined : never) : unknown
        : P extends keyof T ? T[P] : unknown;
    /** Paths whose value is numeric (for $inc, $mul). */
    type DocNumericPaths<T> = { [P in DocPaths<T>]: NonNullable<DocPathValue<T, P>> extends number | bigint ? P : never }[DocPaths<T>];
    /** Paths whose value is an array (for $push, $addToSet, $pull, $pop). */
    type DocArrayPaths<T> = { [P in DocPaths<T>]: NonNullable<DocPathValue<T, P>> extends readonly unknown[] ? P : never }[DocPaths<T>];

    /** BSON type names and numbers accepted by $type (query and expression). */
    type DocBsonType = "double" | "string" | "object" | "array" | "binData" | "undefined" | "objectId" | "bool" | "date" | "null"
        | "regex" | "dbPointer" | "javascript" | "symbol" | "javascriptWithScope" | "int" | "timestamp" | "long" | "decimal"
        | "minKey" | "maxKey" | "number" | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | -1 | 127;
    /** Query operators on one field. Range operators compare in BSON type order ($gt 5 never matches a string). */
    type DocFieldOps<V> = {
        $eq?: V | DocElem<V>; $ne?: V | DocElem<V> | null;
        $gt?: V | DocElem<V>; $gte?: V | DocElem<V>; $lt?: V | DocElem<V>; $lte?: V | DocElem<V>;
        $in?: readonly (V | DocElem<V> | RegExp | null)[]; $nin?: readonly (V | DocElem<V> | RegExp | null)[];
        $exists?: boolean; $type?: DocBsonType | readonly DocBsonType[];
        $regex?: [Extract<V | DocElem<V>, string>] extends [never] ? never : RegExp | string; $options?: string;
        $mod?: [Extract<V | DocElem<V>, number | bigint>] extends [never] ? never : readonly [divisor: number, remainder: number];
        $size?: [DocElem<V>] extends [never] ? never : number;
        $all?: [DocElem<V>] extends [never] ? never : readonly (DocElem<V> | { $elemMatch: DocFilter<DocElem<V>> })[];
        $elemMatch?: [DocElem<V>] extends [never] ? never : DocIsObj<DocElem<V>> extends true ? DocFilter<DocElem<V>> : DocFieldOps<DocElem<V>>;
        $not?: DocFieldOps<V> | RegExp;
    };
    /** A condition on one field: a value (array fields also match one element), a RegExp for strings, or operators. */
    type DocCondition<V> = V | DocElem<V> | (Extract<V | DocElem<V>, string> extends never ? never : RegExp) | DocFieldOps<V> | null;
    /** A query filter over T: dotted paths with conditions plus $and/$or/$nor; an undefined value is skipped. Nesting > 200 levels is RangeError. */
    type DocFilter<T = Doc> = { [P in DocPaths<DocWithId<T>>]?: DocCondition<DocPathValue<DocWithId<T>, P>> } & {
        $and?: readonly DocFilter<T>[]; $or?: readonly DocFilter<T>[]; $nor?: readonly DocFilter<T>[]; $comment?: string;
    };

    type DocPositional<T> = `${DocArrayPaths<T>}.$` | `${DocArrayPaths<T>}.$[]` | `${DocArrayPaths<T>}.$.${string}` | `${DocArrayPaths<T>}.$[].${string}`;
    type DocSetMap<T> = { [P in DocPaths<T>]?: DocPathValue<T, P> } & { [P in DocPositional<T>]?: DocValue };
    type DocPushMod<E> = E | { $each: readonly E[]; $position?: number; $slice?: number; $sort?: 1 | -1 | { [k: string]: 1 | -1 } };
    /** Update operators ($ = first matched array position, $[] = every element; filtered $[<id>] is refused). Fields an
     *  update adds are appended in lexicographic path order; int32 overflow widens to int64, int64 to double. */
    type DocUpdate<T = Doc> = {
        $set?: DocSetMap<T>; $setOnInsert?: DocSetMap<T>;
        $unset?: { [P in DocPaths<T> | DocPositional<T>]?: "" | 1 | true };
        $inc?: { [P in DocNumericPaths<T>]?: number | bigint }; $mul?: { [P in DocNumericPaths<T>]?: number | bigint };
        $min?: DocSetMap<T>; $max?: DocSetMap<T>;
        $rename?: { [P in DocPaths<T>]?: string };
        $push?: { [P in DocArrayPaths<T>]?: DocPushMod<DocElem<NonNullable<DocPathValue<T, P>>>> };
        $addToSet?: { [P in DocArrayPaths<T>]?: DocElem<NonNullable<DocPathValue<T, P>>> | { $each: readonly DocElem<NonNullable<DocPathValue<T, P>>>[] } };
        $pull?: { [P in DocArrayPaths<T>]?: DocCondition<DocElem<NonNullable<DocPathValue<T, P>>>> | DocFilter<DocElem<NonNullable<DocPathValue<T, P>>>> };
        $pullAll?: { [P in DocArrayPaths<T>]?: readonly DocElem<NonNullable<DocPathValue<T, P>>>[] };
        $pop?: { [P in DocArrayPaths<T>]?: 1 | -1 };
        $currentDate?: { [P in DocPaths<T>]?: true | { $type: "date" | "timestamp" } };
    };

    /** Projection: all-inclusion or all-exclusion (only _id may be excluded from an inclusion; mixing is ProjectionMixed 31254). */
    type DocProjection<T = Doc> = { [P in DocPaths<DocWithId<T>>]?: 0 | 1 | boolean };
    type DocTop<P> = P extends `${infer K}.${string}` ? K : P;
    type DocIncluded<Pr> = { [K in keyof Pr]: Pr[K] extends 1 | true ? K : never }[keyof Pr] & string;
    type DocExcluded<Pr> = { [K in keyof Pr]: Pr[K] extends 0 | false ? K : never }[keyof Pr] & string;
    /** The document shape a projection yields: an inclusion keeps the named fields (+ _id unless excluded), an exclusion drops them. */
    type DocProjected<T, Pr> = [DocIncluded<Pr>] extends [never]
        ? Omit<DocWithId<T>, Extract<DocExcluded<Pr>, keyof DocWithId<T>>>
        : Pick<DocWithId<T>, Extract<DocTop<DocIncluded<Pr>> | (Pr extends { _id: 0 | false } ? never : "_id"), keyof DocWithId<T>>>;
    /** Sort or index key pattern: dotted paths with 1 (ascending) or -1 (descending); an index takes up to 32 fields. */
    type DocSort<T = Doc> = { [P in DocPaths<DocWithId<T>>]?: 1 | -1 };

    /** find/findOne/count options; skip/limit are non-negative integers (limit 0 = no limit). */
    interface FindOptions<T = Doc, Pr extends DocProjection<T> = DocProjection<T>> {
        projection?: Pr;
        sort?: DocSort<T>;
        skip?: number;
        limit?: number;
    }
    interface WriteResult { acknowledged: true }
    interface UpdateResult<T = Doc> extends WriteResult {
        matchedCount: number;
        modifiedCount: number;
        upsertedCount: number;
        upsertedId: DocWithId<T>["_id"] | null;
    }
    /** bulkWrite counts; upsertedIds maps operation index to the new _id. */
    interface BulkWriteResult<T = Doc> extends WriteResult {
        insertedCount: number; matchedCount: number; modifiedCount: number; deletedCount: number; upsertedCount: number;
        upsertedIds: Record<number, DocWithId<T>["_id"]>;
    }
    /** One bulkWrite operation (MongoDB driver shape). */
    type DocBulkOp<T = Doc> =
        | { insertOne: { document: DocInsert<T> } }
        | { updateOne: { filter: DocFilter<T>; update: DocUpdate<T>; upsert?: boolean } }
        | { updateMany: { filter: DocFilter<T>; update: DocUpdate<T>; upsert?: boolean } }
        | { replaceOne: { filter: DocFilter<T>; replacement: DocInsert<T>; upsert?: boolean } }
        | { deleteOne: { filter: DocFilter<T> } }
        | { deleteMany: { filter: DocFilter<T> } };
    /** An index spec as listIndexes reports it (_id_ first). */
    interface DocIndexSpec<T = Doc> {
        v: 2; key: DocSort<T>; name: string;
        unique?: boolean; sparse?: boolean; partialFilterExpression?: DocFilter<T>;
    }
    /** explain() output; winningPlan.stage is one of the planner stages. */
    interface DocExplain {
        queryPlanner: {
            namespace: string; parsedQuery: Doc; rejectedPlans: DocPlan[];
            winningPlan: DocPlan;
        };
        executionStats: { nReturned: number; totalKeysExamined: number; totalDocsExamined: number; executionTimeMicros: number };
    }
    interface DocPlan {
        stage: "IDHACK" | "EOF" | "COLLSCAN" | "SORT" | "FETCH" | "IXSCAN" | "AND_SORTED" | "OR";
        inputStage?: DocPlan; inputStages?: DocPlan[]; indexName?: string; keyPattern?: Doc; [detail: string]: unknown;
    }

    /** Error names and codes (MongoDB numbering; 1000xx are dyna:docdb's own). */
    interface DocErrorCodes {
        InternalError: 1; BadValue: 2; FailedToParse: 9; TypeMismatch: 14; NamespaceNotFound: 26; IndexNotFound: 27;
        PathNotViable: 28; ConflictingUpdateOperators: 40; NamespaceExists: 48; ImmutableField: 66; InvalidOptions: 72;
        IndexOptionsConflict: 85; IndexKeySpecsConflict: 86; DocumentValidationFailure: 121; ExceededMemoryLimit: 146;
        CannotIndexParallelArrays: 171; ChangeStreamHistoryLost: 286; BSONObjectTooLarge: 10334; DuplicateKey: 11000;
        PathCollision: 31250; ProjectionMixed: 31254; InvalidSnapshotFile: 100001; SnapshotIOError: 100002; RegexTimeout: 100003;
    }
    type DocErrorName = keyof DocErrorCodes;
    /** Every database-level failure; bulkWrite/insertMany failures also carry writeErrors and the counts that applied.
     *  A $regex runs under a 100M backtracking-step budget (RegexTimeout); a result document over 16 MiB is 10334. */
    class DocDBError extends Error {
        constructor(message?: string, code?: number);
        readonly code: DocErrorCodes[DocErrorName] | 40324 | 168 | 16608 | (number & {});
        readonly codeName: DocErrorName | "UnknownError";
        readonly writeErrors?: { index: number; code: number; codeName: DocErrorName | "UnknownError"; errmsg: string }[];
        readonly result?: Partial<BulkWriteResult>;
    }

    /** "$path" field reference to a dotted path of T. */
    type DocFieldRef<T> = `$${DocPaths<T>}`;
    /** Aggregation variables. */
    type DocVar = "$$ROOT" | "$$CURRENT" | "$$REMOVE" | "$$NOW" | `$$${string}`;
    type DocA<T> = DocExpr<T>;
    /** An aggregation expression over input documents T (operator set of dd-expr.c; date parts are UTC). */
    type DocExpr<T> =
        | DocFieldRef<T> | DocVar | string | number | bigint | boolean | null | Date | ObjectId
        | { $literal: unknown }
        | { $add: readonly DocA<T>[] } | { $subtract: readonly [DocA<T>, DocA<T>] } | { $multiply: readonly DocA<T>[] }
        | { $divide: readonly [DocA<T>, DocA<T>] } | { $mod: readonly [DocA<T>, DocA<T>] } | { $pow: readonly [DocA<T>, DocA<T>] }
        | { $abs: DocA<T> } | { $ceil: DocA<T> } | { $floor: DocA<T> } | { $sqrt: DocA<T> } | { $round: DocA<T> | readonly [DocA<T>, DocA<T>] }
        | { $eq: readonly [DocA<T>, DocA<T>] } | { $ne: readonly [DocA<T>, DocA<T>] } | { $gt: readonly [DocA<T>, DocA<T>] }
        | { $gte: readonly [DocA<T>, DocA<T>] } | { $lt: readonly [DocA<T>, DocA<T>] } | { $lte: readonly [DocA<T>, DocA<T>] }
        | { $cmp: readonly [DocA<T>, DocA<T>] }
        | { $and: readonly DocA<T>[] } | { $or: readonly DocA<T>[] } | { $not: DocA<T> | readonly [DocA<T>] }
        | { $cond: readonly [DocA<T>, DocA<T>, DocA<T>] | { if: DocA<T>; then: DocA<T>; else: DocA<T> } }
        | { $ifNull: readonly DocA<T>[] }
        | { $switch: { branches: readonly { case: DocA<T>; then: DocA<T> }[]; default?: DocA<T> } }
        | { $concat: readonly DocA<T>[] } | { $toLower: DocA<T> } | { $toUpper: DocA<T> } | { $strLenCP: DocA<T> }
        | { $substr: readonly [DocA<T>, DocA<T>, DocA<T>] } | { $substrCP: readonly [DocA<T>, DocA<T>, DocA<T>] }
        | { $split: readonly [DocA<T>, DocA<T>] } | { $trim: { input: DocA<T>; chars?: DocA<T> } }
        | { $size: DocA<T> } | { $arrayElemAt: readonly [DocA<T>, DocA<T>] } | { $first: DocA<T> } | { $last: DocA<T> }
        | { $in: readonly [DocA<T>, DocA<T>] } | { $concatArrays: readonly DocA<T>[] } | { $isArray: DocA<T> | readonly [DocA<T>] }
        | { $slice: readonly [DocA<T>, DocA<T>] | readonly [DocA<T>, DocA<T>, DocA<T>] }
        | { $map: { input: DocA<T>; as?: string; in: DocA<any> } } | { $filter: { input: DocA<T>; as?: string; cond: DocA<any>; limit?: DocA<T> } }
        | { $sum: DocA<T> | readonly DocA<T>[] } | { $avg: DocA<T> | readonly DocA<T>[] }
        | { $min: DocA<T> | readonly DocA<T>[] } | { $max: DocA<T> | readonly DocA<T>[] }
        | { $mergeObjects: DocA<T> | readonly DocA<T>[] } | { $objectToArray: DocA<T> }
        | { $type: DocA<T> } | { $toString: DocA<T> } | { $toInt: DocA<T> } | { $toLong: DocA<T> } | { $toDouble: DocA<T> } | { $toBool: DocA<T> }
        | { $year: DocA<T> } | { $month: DocA<T> } | { $dayOfMonth: DocA<T> } | { $hour: DocA<T> } | { $minute: DocA<T> }
        | { $second: DocA<T> } | { $millisecond: DocA<T> } | { $dayOfWeek: DocA<T> } | { $dayOfYear: DocA<T> }
        | { readonly [field: string]: DocA<T> };
    /** The result type of expression E over T (int+int stays int; $divide and $avg are double). */
    type DocExprType<T, E> =
        E extends "$$ROOT" | "$$CURRENT" ? T : E extends "$$NOW" ? Date : E extends "$$REMOVE" ? undefined
        : E extends `$$${string}` ? unknown
        : E extends `$${infer P}` ? (P extends DocPaths<T> ? DocPathValue<T, P> : unknown)
        : E extends string | number | bigint | boolean | null | Date | ObjectId ? E
        : E extends { $literal: infer L } ? L
        : E extends { $add: any } | { $subtract: any } | { $multiply: any } | { $divide: any } | { $mod: any } | { $pow: any }
            | { $abs: any } | { $ceil: any } | { $floor: any } | { $sqrt: any } | { $round: any } | { $size: any } | { $strLenCP: any }
            | { $cmp: any } | { $sum: any } | { $avg: any } | { $toInt: any } | { $toDouble: any }
            | { $year: any } | { $month: any } | { $dayOfMonth: any } | { $hour: any } | { $minute: any } | { $second: any }
            | { $millisecond: any } | { $dayOfWeek: any } | { $dayOfYear: any } ? number
        : E extends { $toLong: any } ? number | bigint
        : E extends { $eq: any } | { $ne: any } | { $gt: any } | { $gte: any } | { $lt: any } | { $lte: any }
            | { $and: any } | { $or: any } | { $not: any } | { $in: any } | { $isArray: any } | { $toBool: any } ? boolean
        : E extends { $concat: any } | { $toLower: any } | { $toUpper: any } | { $substr: any } | { $substrCP: any }
            | { $trim: any } | { $toString: any } | { $type: any } ? string
        : E extends { $split: any } ? string[]
        : E extends { $cond: readonly [any, infer A, infer B] } ? DocExprType<T, A> | DocExprType<T, B>
        : E extends { $cond: { then: infer A; else: infer B } } ? DocExprType<T, A> | DocExprType<T, B>
        : E extends { $ifNull: readonly [infer A, ...infer R] } ? NonNullable<DocExprType<T, A>> | DocExprType<T, R[number]>
        : E extends { $switch: { branches: readonly { then: infer A }[]; default?: infer D } } ? DocExprType<T, A> | DocExprType<T, D>
        : E extends { $arrayElemAt: readonly [infer A, any] } | { $first: infer A } | { $last: infer A } ? DocElem<DocExprType<T, A>>
        : E extends { $slice: readonly [infer A, ...any[]] } | { $concatArrays: readonly [infer A, ...any[]] } ? DocExprType<T, A>
        : E extends { $min: infer A } | { $max: infer A } ? (A extends readonly unknown[] ? DocExprType<T, A[number]> : DocElem<DocExprType<T, A>> | Exclude<DocExprType<T, A>, readonly unknown[]>)
        : E extends { $map: any } | { $filter: any } | { $concatArrays: any } ? unknown[]
        : E extends { $objectToArray: any } ? { k: string; v: unknown }[]
        : E extends { $mergeObjects: any } ? Doc
        : E extends { readonly [k: string]: any } ? { -readonly [K in keyof E]: DocExprType<T, E[K]> }
        : unknown;

    /** $group accumulators. */
    type DocAccumulator<T> =
        | { $sum: DocExpr<T> } | { $avg: DocExpr<T> } | { $min: DocExpr<T> } | { $max: DocExpr<T> }
        | { $first: DocExpr<T> } | { $last: DocExpr<T> } | { $push: DocExpr<T> } | { $addToSet: DocExpr<T> } | { $count: {} };
    type DocAccType<T, A> =
        A extends { $sum: any } | { $avg: any } | { $count: any } ? number
        : A extends { $min: infer E } | { $max: infer E } | { $first: infer E } | { $last: infer E } ? DocExprType<T, E>
        : A extends { $push: infer E } | { $addToSet: infer E } ? DocExprType<T, E>[]
        : unknown;
    /** $project / $addFields value: 0|1|true|false keeps or drops; anything else is a computed expression. */
    type DocProjectSpec<T> = { readonly [field: string]: 0 | 1 | boolean | DocExpr<T> };
    /** $unwind argument: "$arrayPath" or the long form. */
    type DocUnwind<T> = `$${DocArrayPaths<T>}` | { path: `$${DocArrayPaths<T>}`; includeArrayIndex?: string; preserveNullAndEmptyArrays?: boolean };
    /** One aggregation stage over input documents T (stage set of dd-agg.c; $lookup is the equality form only). */
    type DocStage<T = Doc, S extends DocSchemas = DocAnySchemas> =
        | { $match: DocFilter<T> }
        | { $project: DocProjectSpec<T> }
        | { $addFields: DocProjectSpec<T> } | { $set: DocProjectSpec<T> }
        | { $unset: DocPaths<T> | readonly DocPaths<T>[] }
        | { $unwind: DocUnwind<T> }
        | { $group: { _id: DocExpr<T> } & { readonly [field: string]: DocAccumulator<T> | DocExpr<T> } }
        | { $bucket: { groupBy: DocExpr<T>; boundaries: readonly (number | string | Date)[]; default?: DocValue; output?: { readonly [field: string]: DocAccumulator<T> } } }
        | { $sortByCount: DocExpr<T> }
        | { $sort: DocSort<T> | { readonly [field: string]: 1 | -1 } }
        | { $skip: number } | { $limit: number } | { $count: string }
        | { $lookup: { from: keyof S & string; localField: DocPaths<T>; foreignField: string; as: string } }
        | { $facet: { readonly [output: string]: readonly DocStage<T, S>[] } }
        | { $replaceRoot: { newRoot: DocExpr<T> } } | { $replaceWith: DocExpr<T> };

    type DocUnwound<T, P extends string, A> = Omit<T, DocTop<P>> & {
        [K in DocTop<P>]: P extends `${string}.${string}` ? unknown : A extends { preserveNullAndEmptyArrays: true } ? DocElem<NonNullable<DocPathValue<T, P>>> | null | undefined : DocElem<NonNullable<DocPathValue<T, P>>>;
    } & (A extends { includeArrayIndex: infer I extends string } ? { [K in I]: number | null } : {});
    type DocProjectOut<T, Pr> = [{ [K in keyof Pr]: Pr[K] extends 1 | true ? K : never }[keyof Pr]] extends [never]
        ? [{ [K in keyof Pr]: Pr[K] extends 0 | false ? never : K }[keyof Pr]] extends [never]
            ? Omit<T, Extract<keyof Pr, keyof T>>
            : DocComputed<T, Pr> & ("_id" extends keyof T ? Pr extends { _id: 0 | false } ? {} : { _id: T["_id" & keyof T] } : {})
        : DocComputed<T, Pr> & ("_id" extends keyof T ? Pr extends { _id: 0 | false } ? {} : { _id: T["_id" & keyof T] } : {});
    type DocComputed<T, Pr> = { -readonly [K in keyof Pr as Pr[K] extends 0 | false ? never : K]: Pr[K] extends 1 | true ? (K extends DocPaths<T> ? DocPathValue<T, K & string> : unknown) : DocExprType<T, Pr[K]> };
    type DocGroupOut<T, G> = { -readonly [K in keyof G]: K extends "_id" ? DocExprType<T, G[K]> : DocAccType<T, G[K]> };
    /** The output document type of one stage applied to T. */
    type DocStageOut<T, St, S extends DocSchemas> =
        St extends { $match: any } | { $sort: any } | { $skip: any } | { $limit: any } ? T
        : St extends { $project: infer Pr } ? DocProjectOut<T, Pr>
        : St extends { $addFields: infer Pr } | { $set: infer Pr } ? Omit<T, keyof Pr> & DocComputed<T, Pr>
        : St extends { $unset: infer U } ? Omit<T, Extract<DocTop<U extends readonly (infer X)[] ? X : U>, string>>
        : St extends { $unwind: `$${infer P}` } ? DocUnwound<T, P, {}>
        : St extends { $unwind: { path: `$${infer P}` } & infer A } ? DocUnwound<T, P, A>
        : St extends { $group: infer G } ? DocGroupOut<T, G>
        : St extends { $bucket: { output: infer O } } ? { _id: unknown } & DocGroupOut<T, O>
        : St extends { $bucket: any } ? { _id: unknown; count: number }
        : St extends { $sortByCount: infer E } ? { _id: DocExprType<T, E>; count: number }
        : St extends { $count: infer N extends string } ? { [K in N]: number }
        : St extends { $lookup: { from: infer F; as: infer A extends string } } ? T & { [K in A]: DocWithId<F extends keyof S ? S[F] : Doc>[] }
        : St extends { $facet: infer F } ? { -readonly [K in keyof F]: DocPipelineOut<T, F[K], S>[] }
        : St extends { $replaceRoot: { newRoot: infer E } } | { $replaceWith: infer E } ? DocExprType<T, E>
        : Doc;
    /** The output document type of a whole pipeline (folded stage by stage); a non-literal Stage[] yields Doc. */
    type DocPipelineOut<T, P, S extends DocSchemas = DocAnySchemas> =
        P extends readonly [] ? T
        : P extends readonly [infer St, ...infer R] ? DocPipelineOut<DocStageOut<T, St, S>, R, S>
        : Doc;

    /** A schema map for a Database: collection name -> document type (types $lookup and db.collection(name)). */
    type DocSchemas = object;
    type DocAnySchemas = { [collection: string]: Doc };

    /** A change event: insert/replace carry fullDocument, update carries updateDescription (and fullDocument under
     *  updateLookup, null once deleted), delete only documentKey, drop ns only, invalidate neither; _id is the resume token. */
    type ChangeEvent<T = Doc> = { _id: { _data: string }; wallTime: Date } & (
        | { operationType: "insert" | "replace"; ns: { db: string; coll: string }; documentKey: { _id: DocWithId<T>["_id"] }; fullDocument: DocWithId<T> }
        | { operationType: "update"; ns: { db: string; coll: string }; documentKey: { _id: DocWithId<T>["_id"] }; updateDescription: { updatedFields: { [path: string]: DocValue }; removedFields: string[] }; fullDocument?: DocWithId<T> | null }
        | { operationType: "delete"; ns: { db: string; coll: string }; documentKey: { _id: DocWithId<T>["_id"] } }
        | { operationType: "drop"; ns: { db: string; coll: string } }
        | { operationType: "invalidate" });
    /** watch() options; resumeAfter/startAfter take an event _id or resumeToken and deliver every later event. */
    interface WatchOptions {
        fullDocument?: "default" | "updateLookup";
        resumeAfter?: { _data: string };
        startAfter?: { _data: string };
    }
    /** watch() pipeline stages ($match/$project/$addFields/$set/$unset/$replaceRoot/$replaceWith; others BadValue 2). */
    type DocWatchStage<T = Doc> = Extract<DocStage<ChangeEvent<T>>, { $match: any } | { $project: any } | { $addFields: any } | { $set: any } | { $unset: any } | { $replaceRoot: any } | { $replaceWith: any }>;
    /** An open change stream over a bounded per-collection ring (changeStreamCapacity, default 4096, memory only);
     *  a reader that falls out of the ring, or a token from before recording/a drop/another Database, is HistoryLost 286. */
    class ChangeStream<E = ChangeEvent> implements AsyncIterable<E> {
        /** Streams come from watch(); calling this throws. */
        constructor();
        /** The next event, waiting for a write; { done: true } once closed or invalidated; pending calls resolve in order. */
        next(): Promise<IteratorResult<E>>;
        /** The next ready event or null; throws on HistoryLost; refused while a next() waits. */
        tryNext(): E | null;
        /** Ends the stream; waiting next() calls resolve done. Idempotent. A pending next() blocks Database.close(). */
        close(): void;
        return(): Promise<IteratorResult<E>>;
        /** Token of the last event read (or the start point), for watch([], { resumeAfter }). */
        readonly resumeToken: { _data: string };
        readonly closed: boolean;
        [Symbol.asyncIterator](): ChangeStream<E>;
    }

    /** Snapshot result. File: header {"DYNADOC\0", v1, created ms, sections, CRC32C} + sections {kind, length, CRC32C}
     *  + footer {"DYNAEND\0", length, count, CRC32C of CRCs}; indexes are rebuilt on load; any damage is 100001. */
    interface PersistResult { path: string; bytes: number; collections: number; documents: number }

    /** A collection of T documents; created by its first write or createCollection. Reads of a missing collection, and
     *  writes that change nothing, answer as empty and do not create it (dropIndex there is NamespaceNotFound 26). */
    class Collection<T extends object = Doc, S extends DocSchemas = DocAnySchemas> {
        /** Collections come from db.collection(name); calling this throws. */
        constructor();
        readonly name: string;
        /** Adds an ObjectId _id when absent (the argument is not modified). */
        insertOne(doc: DocInsert<T>): WriteResult & { insertedId: DocWithId<T>["_id"] };
        /** ordered (default true) stops at the first failure; any failure throws DocDBError with writeErrors after applying the rest. */
        insertMany(docs: readonly DocInsert<T>[], opts?: { ordered?: boolean }): WriteResult & { insertedCount: number; insertedIds: Record<number, DocWithId<T>["_id"]> };
        /** Matches in sort order (else slot order) after skip/limit; a sort+limit on an indexed field walks the index and stops early. */
        find<const Pr extends DocProjection<T> = {}>(filter?: DocFilter<T>, opts?: FindOptions<T, Pr>): DocProjected<T, Pr>[];
        findOne<const Pr extends DocProjection<T> = {}>(filter?: DocFilter<T>, opts?: FindOptions<T, Pr>): DocProjected<T, Pr> | null;
        /** Matches after skip/limit; one $eq/$in/range on a single-field non-multikey index counts postings without reading documents. */
        countDocuments(filter?: DocFilter<T>, opts?: { skip?: number; limit?: number }): number;
        estimatedDocumentCount(): number;
        /** Distinct values of a dotted field among the matches, array elements individually, first-seen order. */
        distinct<const P extends DocPaths<DocWithId<T>>>(field: P, filter?: DocFilter<T>): (DocElem<DocPathValue<DocWithId<T>, P>> | Exclude<DocPathValue<DocWithId<T>, P>, readonly unknown[]>)[];
        /** Updates the first match (in sort order); upsert inserts the filter's equality fields with the update applied. */
        updateOne(filter: DocFilter<T>, update: DocUpdate<T>, opts?: { upsert?: boolean; sort?: DocSort<T> }): UpdateResult<T>;
        /** Updates every match; a unique-index violation stops there with earlier documents applied. */
        updateMany(filter: DocFilter<T>, update: DocUpdate<T>, opts?: { upsert?: boolean }): UpdateResult<T>;
        /** Replaces the first match keeping its _id (a different _id is ImmutableField 66). */
        replaceOne(filter: DocFilter<T>, replacement: DocInsert<T>, opts?: { upsert?: boolean }): UpdateResult<T>;
        deleteOne(filter: DocFilter<T>): WriteResult & { deletedCount: number };
        deleteMany(filter: DocFilter<T>): WriteResult & { deletedCount: number };
        /** Updates the first match in sort order; returns it before (default) or after, or null. */
        findOneAndUpdate<const Pr extends DocProjection<T> = {}>(filter: DocFilter<T>, update: DocUpdate<T>, opts?: FindOptions<T, Pr> & { upsert?: boolean; returnDocument?: "before" | "after" }): DocProjected<T, Pr> | null;
        findOneAndReplace<const Pr extends DocProjection<T> = {}>(filter: DocFilter<T>, replacement: DocInsert<T>, opts?: FindOptions<T, Pr> & { upsert?: boolean; returnDocument?: "before" | "after" }): DocProjected<T, Pr> | null;
        /** Deletes the first match in sort order and returns it, or null. */
        findOneAndDelete<const Pr extends DocProjection<T> = {}>(filter: DocFilter<T>, opts?: FindOptions<T, Pr>): DocProjected<T, Pr> | null;
        /** ordered (default true) stops at the first failure. */
        bulkWrite(ops: readonly DocBulkOp<T>[], opts?: { ordered?: boolean }): BulkWriteResult<T>;
        /** Builds an index (up to 32 fields) and returns its name (default "a_1_b_-1"); same spec again is a no-op, same name other spec 85/86. */
        createIndex(keys: DocSort<T>, opts?: { name?: string; unique?: boolean; sparse?: boolean; partialFilterExpression?: DocFilter<T> }): string;
        /** By name or key pattern; the implicit _id_ index cannot be dropped. */
        dropIndex(nameOrKeys: string | DocSort<T>): void;
        listIndexes(): DocIndexSpec<T>[];
        /** Plans and runs the query; reports the winning and rejected plans and execution stats. */
        explain(filter?: DocFilter<T>, opts?: FindOptions<T>): DocExplain;
        /** Runs a pipeline; output type is inferred stage by stage. A leading $match/$sort/$skip/$limit uses indexes; inputs of
         *  2048+ documents run on the pool unless a $match uses $regex. Every failure rejects (40324 stage, 168 operator, 16608 /0). */
        aggregate<const P extends readonly DocStage<DocWithId<T>, S>[]>(pipeline: P): Promise<DocPipelineOut<DocWithId<T>, P, S>[]>;
        /** A change stream over this collection (need not exist yet), from now or after a resume token. */
        watch(pipeline?: readonly DocWatchStage<T>[], opts?: WatchOptions): ChangeStream<ChangeEvent<T>>;
        /** Snapshot of this collection alone. */
        persist(path: string, opts?: { fsync?: boolean }): Promise<PersistResult>;
        /** Drops the collection and its indexes; false when it did not exist. */
        drop(): boolean;
    }
    /** An in-memory database; S maps collection names to document types. close() is refused while persist/load runs. */
    class Database<S extends DocSchemas = DocAnySchemas> implements DynResource {
        /** name defaults to "test"; changeStreamCapacity bounds each collection's event ring (default 4096). */
        constructor(name?: string, opts?: { changeStreamCapacity?: number });
        /** Reads a snapshot into a new Database (name restored); any damage rejects InvalidSnapshotFile 100001. */
        static load<S extends DocSchemas = DocAnySchemas>(path: string): Promise<Database<S>>;
        readonly name: string;
        /** A handle on a collection, which need not exist yet; typed by S, or explicitly with collection<T>(name). */
        collection<N extends keyof S & string>(name: N): Collection<S[N] & object, S>;
        collection<T extends object = Doc>(name: string): Collection<T, S>;
        /** Creates an empty collection (NamespaceExists 48 when present); options are persisted. */
        createCollection<N extends keyof S & string>(name: N, opts?: { changeStreamCapacity?: number }): Collection<S[N] & object, S>;
        createCollection<T extends object = Doc>(name: string, opts?: { changeStreamCapacity?: number }): Collection<T, S>;
        listCollections(): (keyof S & string)[];
        dropCollection(name: keyof S & string | string): boolean;
        /** Every collection to one snapshot file off the JS thread; later writes are not in it. */
        persist(path: string, opts?: { fsync?: boolean }): Promise<PersistResult>;
        /** Replaces the database with a snapshot (merge: true replaces only the collections the file holds). */
        load(path: string, opts?: { merge?: boolean }): Promise<PersistResult>;
        /** A stream over every collection in write order; a drop is a "drop" event and does not end it. */
        watch(pipeline?: readonly DocWatchStage<S[keyof S]>[], opts?: WatchOptions): ChangeStream<ChangeEvent<S[keyof S] & object>>;
        stats(): { collections: number; documents: number; dataBytes: number; indexes: number; indexEntries: number; retiredBytes: number; pinnedSnapshots: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}