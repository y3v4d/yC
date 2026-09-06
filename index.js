import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const wasmBuffer = fs.readFileSync(path.join(__dirname, "build/out.wasm"));

WebAssembly.instantiate(wasmBuffer, {
    core: {
        print: (arg) => {
            console.log(arg);
        }
    }
});
