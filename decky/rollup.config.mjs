import commonjs from '@rollup/plugin-commonjs';
import resolve from '@rollup/plugin-node-resolve';
import typescript from '@rollup/plugin-typescript';

export default {
  input: 'src/index.tsx',
  output: {
    file: 'dist/index.js',
    format: 'esm',
    sourcemap: false
  },
  plugins: [
    resolve({
      browser: true
    }),
    commonjs(),
    typescript({
      compilerOptions: {
        jsx: 'react-jsx',
        target: 'es2020',
        module: 'esnext',
        moduleResolution: 'node',
        allowSyntheticDefaultImports: true,
        esModuleInterop: true
      }
    })
  ],
  external: ['react', 'react/jsx-runtime', 'decky-frontend-lib']
};
