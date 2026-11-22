import * as path from 'path';
import Mocha = require('mocha');

export function run(): Promise<void> {
    const mocha = new Mocha({ ui: 'bdd', color: true });
    const testsRoot = path.resolve(__dirname);

    mocha.addFile(path.resolve(testsRoot, 'smoke.test.js'));

    return new Promise((resolve, reject) => {
        try {
            mocha.run((failures: number) => {
                if (failures > 0) {
                    reject(new Error(`${failures} test(s) failed`));
                } else {
                    resolve();
                }
            });
        } catch (e) {
            reject(e);
        }
    });
}