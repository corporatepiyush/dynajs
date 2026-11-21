/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:ml
/** dyna:ml — models in C over contiguous doubles: linear/logistic regression, trees, forests, boosting, SVC, k-NN, k-means, DBSCAN, GMM, PCA, scalers, Pipeline, model selection. Estimators share fit/predict/serialize and are native resources; fit() refuses non-finite X (XGB* treat NaN as missing) and honours --timeout-ms.
 *  Every fit/predict/transform, metric, splitter and search returns a Promise: arguments are read and validated on the calling thread, the computation runs on a thread created for that one call and destroyed when it ends (no pool), and the result is built back on the calling thread. Bad arguments reject the promise; they never throw synchronously. `save` (encode, write, durable flush) and `load` (read, decode) are calls of the same kind. Constructors, getters, serialize/deserialize (in memory, no file I/O), `CSR.row` and `Pipeline.stage` stay synchronous. Using a model before fit() (predict, predictProba, transform, apply, featureImportances, and LogisticRegression coef/intercept/classes) rejects or throws a plain Error "<Model>.<op>: call fit() first". */
declare module "dyna:ml" {
    /** Inputs are snapshotted when the call starts: writing to, resizing or transferring X, y or weights afterwards cannot change the result. Reading nested number[][] rows happens on the calling thread; a flat Float64Array is one copy. One object, one writer: `fit`/`fitTransform` need the object to themselves and reject with TypeError while any other call on it is unsettled; reads (`predict`, `transform`, ...) may overlap each other; getters throw while a fit is in flight, and `close()` throws while any call is unsettled.
     *  At most 128 compute threads are alive at once per JS thread; a call beyond that waits its turn and still gets a thread of its own. threadStats: threads created, calls that ran inline because a thread could not be created, and threads alive now. */
    function threadStats(): { spawned: number; inline: number; live: number };
    /** A matrix as an array of rows, or a flat Float64Array plus (rows, cols). */
    type Matrix = number[][] | Float64Array;
    /** Targets or labels, one per row. */
    type Target = number[] | Float64Array;

    /** Shared estimator options. */
    interface TreeOpts {
        /** Trees (forest) or boosting rounds; default 100. Fit time is linear in it; above 100000 is refused at construction. */
        nEstimators?: number;
        /** 0 or absent means the 1024 hard ceiling; above 1024 is REFUSED (trees grow by C recursion). */
        maxDepth?: number;
        minSamplesSplit?: number;
        minSamplesLeaf?: number;
        maxFeatures?: number;
        /** 0 (default) selects the EXACT splitter; above 0 the HISTOGRAM splitter with at most this many bins per feature: much faster, slightly coarser (255 is typical). */
        maxBins?: number;
        seed?: number;
    }
    /** Trailing fit() options: `sampleWeight` gives one weight per row. */
    interface FitOpts {
        sampleWeight?: number[] | Float64Array;
    }

    /** Closed-form ordinary least squares; a CSR X gives bit-identical coefficients to the dense fit. */
    class LinearRegression implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): LinearRegression;
        static load(path: import("dyna:file").Path): Promise<LinearRegression>;
        /** The ridge scales with the mean weight, so scaling all weights leaves the fit unchanged. */
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Array of fitted weights (`[]` before fit). */
        readonly coef: number[];
        /** Fitted intercept (`0` before fit). */
        readonly intercept: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Full-batch gradient descent logistic regression. */
    class LogisticRegression implements DynResource {
        /** Defaults: learningRate 0.1, maxIter 3000, tol 1e-4, l1 = l2 = 0; `C` is the inverse penalty strength used with `penalty` (scikit-learn's convention). */
        constructor(opts?: { learningRate?: number; maxIter?: number; tol?: number; l1?: number; l2?: number; C?: number; penalty?: "l1" | "l2" | "elasticnet" | "none"; classWeight?: "balanced" });
        static deserialize(bytes: Uint8Array | ArrayBuffer): LogisticRegression;
        static load(path: import("dyna:file").Path): Promise<LogisticRegression>;
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** Distinct labels, ascending. */
        readonly classes: number[];
        /** Weight matrix (1 row binary, one per class multinomial). */
        readonly coef: number[][];
        /** Scalar (binary) or Array (multinomial). */
        readonly intercept: number | number[];
        /** Iterations run. */
        readonly nIter: number;
        /** Whether the gradient-norm stop fired. */
        readonly converged: boolean;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Lloyd's algorithm with k-means++ seeding. */
    class KMeans implements DynResource {
        /** nClusters defaults to 8. A negative seed keeps the non-reproducible default; pass a non-negative integer (or `{ seed }`) for reproducibility. */
        constructor(nClusters?: number, seed?: number | { seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): KMeans;
        static load(path: import("dyna:file").Path): Promise<KMeans>;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Summed squared distance to assigned centroids of the last fit (`0` before fit). */
        readonly inertia: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** SMO support-vector classifier. */
    class SVC implements DynResource {
        /** Defaults: kernel "rbf", C 1, gamma 1/cols, coef0 0, degree 3, tol 1e-3, maxIter 1000; more than two classes train one-vs-rest (up to 256). */
        constructor(opts?: { kernel?: "linear" | "rbf" | "poly"; C?: number; gamma?: number; coef0?: number; degree?: number; tol?: number; maxIter?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): SVC;
        static load(path: import("dyna:file").Path): Promise<SVC>;
        /** Needs at least two distinct labels. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        decisionFunction(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[] | number[][] | Float64Array>;
        /** Total support vectors across all binary machines (`0` before fit). */
        readonly nSupportVectors: number;
        /** Labels in ascending order. */
        readonly classes: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** EM soft clustering over diagonal-covariance Gaussians. */
    class GaussianMixture implements DynResource {
        /** Defaults: k 3, seed 12345, maxIter 200, tol 1e-3, regCovar 1e-6 (added to every variance so a component cannot collapse). */
        constructor(k?: number, opts?: { seed?: number; maxIter?: number; tol?: number; regCovar?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianMixture;
        static load(path: import("dyna:file").Path): Promise<GaussianMixture>;
        /** Needs at least `k` rows. */
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Mixing weights (sum 1). */
        readonly weights: number[];
        readonly means: number[][];
        /** K×cols, regularised. */
        readonly variances: number[][];
        /** Total log-likelihood of the last fit (`0` before fit). */
        readonly logLikelihood: number;
        /** EM iterations run. */
        readonly nIter: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Naive Bayes with per-class Gaussian densities. */
    class GaussianNB implements DynResource {
        /** varSmoothing defaults to 1e-9, scaled by the largest variance (scikit-learn's floor). */
        constructor(varSmoothing?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianNB;
        static load(path: import("dyna:file").Path): Promise<GaussianNB>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Labels seen at fit. */
        readonly classes: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** CART classifier over Gini impurity. */
    class DecisionTreeClassifier implements DynResource {
        /** TreeOpts defaults: maxDepth 0 (unlimited up to 1024), minSamplesSplit 2, minSamplesLeaf 1, maxBins 0 (exact), seed 12345; a single tree ignores nEstimators. */
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeClassifier;
        static load(path: import("dyna:file").Path): Promise<DecisionTreeClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature, normalised to sum 1 (zeros when nothing ever split). */
        readonly featureImportances: number[];
        /** Deepest tree (0 before fit). */
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** CART regressor minimising variance. */
    class DecisionTreeRegressor implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeRegressor;
        static load(path: import("dyna:file").Path): Promise<DecisionTreeRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        /** Deepest tree (0 before fit). */
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Bagged decision trees; a fixed seed reproduces a forest exactly. */
    class RandomForestClassifier implements DynResource {
        /** TreeOpts defaults: nEstimators 100, maxDepth 0, minSamplesSplit 2, minSamplesLeaf 1, maxBins 0 (exact), seed 12345. */
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestClassifier;
        static load(path: import("dyna:file").Path): Promise<RandomForestClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Bagged regression trees. */
    class RandomForestRegressor implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestRegressor;
        static load(path: import("dyna:file").Path): Promise<RandomForestRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for regression. Shared options (nEstimators <= 100000, maxDepth <= 1024, maxBins, seed) follow TreeOpts. */
    class GradientBoostingRegressor implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 3, learningRate 0.1, subsample 1, seed 12345. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingRegressor;
        static load(path: import("dyna:file").Path): Promise<GradientBoostingRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for classification. Shared options (nEstimators <= 100000, maxDepth <= 1024, maxBins, seed) follow TreeOpts. */
    class GradientBoostingClassifier implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 3, learningRate 0.1, subsample 1, seed 12345. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingClassifier;
        static load(path: import("dyna:file").Path): Promise<GradientBoostingClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Second-order (Newton) boosting; NaN means "missing". */
    class XGBRegressor implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 6, learningRate 0.3, lambda 1, alpha 0, gamma 0, minChildWeight 1, validationFraction 0.1, earlyStoppingRounds 0. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBRegressor;
        static load(path: import("dyna:file").Path): Promise<XGBRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        /** The rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit). */
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** The classifier twin of XGBRegressor. */
    class XGBClassifier implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 6, learningRate 0.3, lambda 1, alpha 0, gamma 0, minChildWeight 1, validationFraction 0.1, earlyStoppingRounds 0. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBClassifier;
        static load(path: import("dyna:file").Path): Promise<XGBClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        /** The rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit). */
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Principal components by cyclic Jacobi; fit() refuses (RangeError) when 60 * nFeatures^3 exceeds 2^36 (~1100 features). */
    class PCA implements DynResource {
        /** nComponents 0 (default) keeps every feature; `whiten` defaults to false. */
        constructor(nComponents?: number, whiten?: boolean);
        static deserialize(bytes: Uint8Array | ArrayBuffer): PCA;
        static load(path: import("dyna:file").Path): Promise<PCA>;
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /** Centred rows projected onto the components. */
        transform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is a flat Float64Array (rows x cols); nested input yields number[][]. */
        fitTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** The projection back into feature space (used with `nComponents` < features to reconstruct). */
        inverseTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** NComponents × features (unit rows). */
        readonly components: number[][];
        /** Per-feature training mean. */
        readonly mean: number[];
        /** Eigenvalues of the sample (n-1) covariance, retained components. */
        readonly explainedVariance: number[];
        /** Each divided by the total variance (sums to ~1). */
        readonly explainedVarianceRatio: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours classifier; lazy, distances stay squared. */
    class KNClassifier implements DynResource {
        /** k defaults to 5; weights is "uniform" (default) or "distance". */
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNClassifier;
        static load(path: import("dyna:file").Path): Promise<KNClassifier>;
        /** Requires at least `k` rows. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours regressor. */
    class KNRegressor implements DynResource {
        /** k defaults to 5; weights is "uniform" (default) or "distance". */
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNRegressor;
        static load(path: import("dyna:file").Path): Promise<KNRegressor>;
        /** Requires at least `k` rows. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Density-based clustering with a grid index for low-dimensional data. */
    class DBScan implements DynResource {
        /** eps defaults to 0.5, minPts to 5. */
        constructor(eps?: number, minPts?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DBScan;
        static load(path: import("dyna:file").Path): Promise<DBScan>;
        /** No weighted form; `sampleWeight` throws. */
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /** Array of per-row cluster ids, `-1` for noise (`[]` before fit). */
        readonly labels: number[];
        /** Cluster count (0 before fit). */
        readonly nClusters: number;
        /** The eps the model was constructed with. */
        readonly eps: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Per-column z-score scaling; constant columns report std 1.0. */
    class StandardScaler implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): StandardScaler;
        static load(path: import("dyna:file").Path): Promise<StandardScaler>;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /** A FLAT Float64Array comes back when X is a flat Float64Array (rows x cols); nested input yields number[][]. */
        transform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        fitTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** `x·std + mean`. Output shape mirrors the input shape. */
        inverseTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Per-feature means. */
        readonly mean: number[];
        /** Per-feature standard deviations (`1.0` for constant columns). */
        readonly std: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Per-column min-max scaling to [0, 1]. */
    class MinMaxScaler implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): MinMaxScaler;
        static load(path: import("dyna:file").Path): Promise<MinMaxScaler>;
        /** Refuses `sampleWeight` (min and max are order statistics no positive weight can change). */
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /** A FLAT Float64Array comes back when X is flat. */
        transform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        fitTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** `x·(max−min) + min`. Output shape mirrors the input shape. */
        inverseTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Per-column minima. */
        readonly dataMin: number[];
        /** Per-column maxima. */
        readonly dataMax: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An immutable compressed sparse row matrix (scipy layout). Every predict* accepts one and matches the dense answer bit for bit; only Linear/LogisticRegression fit one.
     *  Duplicate columns in a row SUM; the ctor validates everything: TypeError on shape/length mismatch, RangeError otherwise. */
    class CSR implements DynResource {
        constructor(values: number[] | Float64Array, columns: number[] | Int32Array | Uint32Array, rowPointers: number[] | Int32Array | Uint32Array, cols: number);
        /** Drops exact zeros from a dense matrix; a non-finite value is a RangeError naming the cell. */
        static fromDense(X: Matrix, rows?: number, cols?: number): Promise<CSR>;
        /** The dense form (duplicate columns sum); throws if it does not fit in memory. */
        toDense(): Promise<number[][]>;
        /** Row i as a dense Array (duplicate columns sum). */
        row(i: number): number[];
        readonly rows: number;
        readonly cols: number;
        /** Nonzero count. */
        readonly nnz: number;
        /** `nnz / (rows·cols)`. */
        readonly density: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** DBScan is NOT a PipelineStage: it fits but cannot transform/predict, so a Pipeline has no position for it. */
    type PipelineStage = LinearRegression | LogisticRegression | KMeans | SVC
        | GaussianMixture | GaussianNB | DecisionTreeClassifier | DecisionTreeRegressor
        | RandomForestClassifier | RandomForestRegressor | GradientBoostingRegressor
        | GradientBoostingClassifier | XGBRegressor | XGBClassifier | PCA
        | KNClassifier | KNRegressor | StandardScaler | MinMaxScaler;

    /** A composition of feature stages and a final estimator. */
    class Pipeline implements DynResource {
        constructor(stages: PipelineStage[]);
        fit(X: Matrix, y: Target): Promise<this>;
        /** Pushes X through the stages, ask the last one. */
        predict(X: Matrix): Promise<number[]>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix): Promise<number[][]>;
        /** Pushes through every stage that can transform, stopping before a final bare estimator. */
        transform(X: Matrix): Promise<number[][]>;
        /** The i-th stage (negative counts from the end). */
        stage(i: number): PipelineStage;
        readonly length: number;
        readonly fitted: boolean;
        /** The final stage. */
        readonly estimator: PipelineStage;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Mean of squared errors. */
    function meanSquaredError(yTrue: Target, yPred: Target): Promise<number>;
    /** The mean absolute error. */
    function meanAbsoluteError(yTrue: Target, yPred: Target): Promise<number>;
    /** Coefficient of determination; a constant yTrue scores 1.0 for exact, else 0.0. */
    function r2Score(yTrue: Target, yPred: Target): Promise<number>;
    /** The fraction of equal pairs. */
    function accuracy(yTrue: Target, yPred: Target): Promise<number>;
    /** Mean negative log-likelihood; yPred may be a class matrix or a binary vector. */
    function logLoss(yTrue: Target, yPred: Target | Target[]): Promise<number>;
    /** Confusion matrix indexed [true][pred]; labels are integers 0..4095 (dense nLabels x nLabels). */
    function confusionMatrix(yTrue: Target, yPred: Target): Promise<number[][]>;
    /** Binary-classification metrics; `positive` names the positive class label. */
    function precision(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The recall. Defaults: positive 1. */
    function recall(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The F1 score. Defaults: positive 1. */
    function f1(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The specificity. Defaults: positive 1. */
    function specificity(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** `(recall + specificity)/2`. Defaults: positive 1. */
    function balancedAccuracy(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The Matthews correlation coefficient, the one number that stays honest on imbalanced data. Defaults: positive 1. */
    function matthewsCorrcoef(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** Agreement over chance. Defaults: positive 1. */
    function cohenKappa(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** beta > 1 weights recall. */
    function fbeta(yTrue: Target, yPred: Target, beta: number, positive?: number): Promise<number>;
    /** Exact Mann-Whitney U; lacking a positive or negative example is a RangeError; `positive` defaults to the largest label. */
    function rocAuc(yTrue: Target, yScore: Target, positive?: number): Promise<number>;
    /** Area under the precision-recall curve, from scores. */
    function averagePrecision(yTrue: Target, yScore: Target, positive?: number): Promise<number>;

    /** Indices, not data. testSize is a FRACTION in (0,1), default 0.25; the count form (testSize: 10) is a RangeError. */
    function trainTestSplit(n: number | Target, opts?: { testSize?: number; shuffle?: boolean; seed?: number }): Promise<{ train: number[]; test: number[] }>;
    /** Default 5 folds; when both `k` and `folds` are given, `folds` wins. */
    function kFold(n: number | Target, opts?: { k?: number; folds?: number; shuffle?: boolean; seed?: number }): Promise<{ train: number[]; test: number[] }[]>;
    /** K folds that preserve each class's proportion: each class's members are dealt round-robin to folds 0..k-1 in row order,
     *  or, with shuffle, in the order of a seeded permutation, so different seeds give different folds (shuffle:false is seed-independent). */
    function stratifiedKFold(y: Target, opts?: { k?: number; shuffle?: boolean; seed?: number }): Promise<{ train: number[]; test: number[] }[]>;
    /** Cross-validated scores, one per fold; estimatorFactory is () => new Model(...). `nJobs` is validated but folds run one after another (each fit and predict on its own thread); `onProgress({done, total})` fires after each fold. */
    function crossValScore(estimatorFactory: () => { fit(X: Matrix, y: Target): unknown; predict(X: Matrix): number[]; close(): void }, X: Matrix, y: Target, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): Promise<number[]>;
    /** Exhaustive parameter search. */
    function gridSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): Promise<{ best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] }>;
    /** Random parameter search over nIter sampled points. */
    function randomSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { nIter?: number; k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): Promise<{ best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] }>;

    /** Replaces every non-finite entry with its column's finite mean; a column with NO finite value is a RangeError naming it. */
    function imputeMean(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
    /** Removes rows holding a non-finite value; `kept` lists the survivors. */
    function dropMissing(X: Matrix, y?: Target, rows?: number, cols?: number): Promise<{ X: number[][]; y: Float64Array | undefined; kept: number[] }>;
}