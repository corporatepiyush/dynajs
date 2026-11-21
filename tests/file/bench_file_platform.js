import { dataDir, configDir, cacheDir, dataDirSite, configDirSite,
         cacheDirSite } from "dyna:file";

const N = 200000;

function bestOf(fn) {
    let b = Infinity;
    for (let t = 0; t < 5; t++) {
        const t0 = performance.now();
        for (let i = 0; i < N; i++) fn();
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b * 1e6 / N;
}

const floor = bestOf(() => {});
print(`#DATA\tpdir_floor\t${floor.toFixed(3)}`);
print(`#DATA\tdataDir\t${bestOf(() => dataDir()).toFixed(3)}`);
print(`#DATA\tconfigDir\t${bestOf(() => configDir()).toFixed(3)}`);
print(`#DATA\tcacheDir\t${bestOf(() => cacheDir()).toFixed(3)}`);
print(`#DATA\tdataDirSite\t${bestOf(() => dataDirSite()).toFixed(3)}`);
print(`#DATA\tconfigDirSite\t${bestOf(() => configDirSite()).toFixed(3)}`);
print(`#DATA\tcacheDirSite\t${bestOf(() => cacheDirSite()).toFixed(3)}`);
print(`#DATA\tdataDir_app\t${bestOf(() => dataDir("bench")).toFixed(3)}`);
