import { defineConfig } from 'vite';
import { VitePWA } from 'vite-plugin-pwa';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig({
    plugins: [
        tailwindcss(),
        VitePWA({
            registerType: 'autoUpdate',
            devOptions: { enabled: false },
            manifest: {
                name: 'Ravendrel Quantum AI',
                short_name: 'Ravendrel',
                description: 'Local Interface for QKM Inference',
                theme_color: '#0f172a',
                background_color: '#0f172a',
                display: 'standalone',
                start_url: '/',
                icons: [{ src: '/favicon.svg', sizes: 'any', type: 'image/svg+xml' }],
            },
        }),
    ],
});
