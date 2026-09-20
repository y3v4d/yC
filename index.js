import fs from "fs";
import path from "path";
import { stdout } from "process";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const wasmBuffer = fs.readFileSync(path.join(__dirname, "build/out.wasm"));

async function loadWasm() {
    const { instance }= await WebAssembly.instantiate(wasmBuffer, {
        core: {
            print: (arg) => {
                console.log(arg);
            },
            putc: (arg) => {
                stdout.write(String.fromCharCode(arg));
            },
            puts: (arg) => {
                let length = 0;
                const bytes = new Uint8Array(instance.exports.memory.buffer, arg);
                for (let i = 0; ; i++) {
                    if (bytes[i] === 0) {
                        break;
                    }
                    length++;
                }

                const bytesToDecode = bytes.slice(0, length);
                const str = new TextDecoder("utf-8").decode(bytesToDecode);

                console.log(str);
            }
        }
    });

    instance.exports.main();
}

loadWasm();
