import tseslint from 'typescript-eslint';
import eslintConfigPrettier from 'eslint-config-prettier';
import globals from 'globals';

export default tseslint.config(
    ...tseslint.configs.recommended,
    eslintConfigPrettier,
    {
        languageOptions: {
            globals: {
                ...globals.browser,
            },
        },
    },
    {
        ignores: ['dist/**', 'node_modules/**', 'dev-dist/**', 'src/**/*.test.ts'],
    }
);
