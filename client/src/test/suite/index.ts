import * as path from 'path';
import Mocha = require('mocha');

// mocha suite 运行器:加载 suite 目录下的测试文件。
export function run(): Promise<void> {
    const mocha = new Mocha({ ui: 'bdd', color: true });
    const testsRoot = path.resolve(__dirname);

    // 显式列出测试文件,避免依赖 glob
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
