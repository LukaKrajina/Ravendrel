#include "i18n.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>

namespace qgui
{

    namespace
    {

    struct Entry
    {
        const char *key;
        const char *en;
        const char *zh;
        const char *ru;
        const char *fr;
        const char *de;
    };

    // 字典（key 用英文原文，便于代码内以英文为基准调用）
    const Entry kEntries[] = {
        {"Metrics", "Metrics", "指标", "Метрики", "Métriques", "Metriken"},
        {"State Vector", "State Vector", "状态向量", "Вектор состояния", "Vecteur d'état", "Zustandsvektor"},
        {"Bloch Sphere", "Bloch Sphere", "布洛赫球", "Сфера Блоха", "Sphère de Bloch", "Bloch-Sphäre"},
        {"Quantum Objects", "Quantum Objects", "量子对象", "Квантовые объекты", "Objets quantiques", "Quantenobjekte"},
        {"Measurement History", "Measurement History", "测量历史", "История измерений", "Historique des mesures", "Messverlauf"},
        {"Settings", "Settings", "设置", "Настройки", "Paramètres", "Einstellungen"},
        {"Language", "Language", "语言", "Язык", "Langue", "Sprache"},
        {"No objects", "No objects", "无对象", "Нет объектов", "Aucun objet", "Keine Objekte"},
        {"Type", "Type", "类型", "Тип", "Type", "Typ"},
        {"Qubit ids", "Qubit ids", "量子比特 ID", "ID кубитов", "ID des qubits", "Qubit-IDs"},
        {"Waiting for state data...", "Waiting for state data...", "等待状态数据...", "Ожидание данных состояния...", "En attente des données d'état...", "Warte auf Zustandsdaten..."},
        {"No measurements yet", "No measurements yet", "尚无测量结果", "Пока нет измерений", "Aucune mesure pour l'instant", "Noch keine Messungen"},
        {"connected", "connected", "已连接", "подключено", "connecté", "verbunden"},
        {"OFFLINE", "OFFLINE", "离线", "ОФФЛАЙН", "HORS LIGNE", "OFFLINE"},
        {"Qubit: %d", "Qubit: %d", "量子比特: %d", "Кубит: %d", "Qubit : %d", "Qubit: %d"},
        {"%u qubits, %zu basis states (gen %llu)", "%u qubits, %zu basis states (gen %llu)", "%u 个量子比特, %zu 个基态 (第 %llu 代)", "%u кубитов, %zu базисных состояний (поколение %llu)", "%u qubits, %zu états de base (génération %llu)", "%u Qubits, %zu Basiszustände (Gen %llu)"},
        {"FPS: %.1f   frame: %.2f ms", "FPS: %.1f   frame: %.2f ms", "帧率: %.1f   帧时间: %.2f 毫秒", "FPS: %.1f   кадр: %.2f мс", "FPS : %.1f   trame : %.2f ms", "FPS: %.1f   Frame: %.2f ms"},
        {"Daemon: %s   generation: %llu", "Daemon: %s   generation: %llu", "守护进程: %s   代数: %llu", "Демон: %s   поколение: %llu", "Démon : %s   génération : %llu", "Daemon: %s   Generation: %llu"},
        {"Backend: %s", "Backend: %s", "后端: %s", "Бэкенд: %s", "Backend : %s", "Backend: %s"},
        {"Qubits: %u   amplitudes: %zu", "Qubits: %u   amplitudes: %zu", "量子比特: %u   振幅: %zu", "Кубиты: %u   амплитуды: %zu", "Qubits : %u   amplitudes : %zu", "Qubits: %u   Amplituden: %zu"},
        {"Present mode: %s", "Present mode: %s", "呈现模式: %s", "Режим представления: %s", "Mode de présentation : %s", "Present-Modus: %s"},
        {"%zu samples: |0> = %d, |1> = %d", "%zu samples: |0> = %d, |1> = %d", "%zu 个样本: |0> = %d, |1> = %d", "%zu образцов: |0> = %d, |1> = %d", "%zu échantillons : |0> = %d, |1> = %d", "%zu Samples: |0> = %d, |1> = %d"},
        {"Last %zu: |0> = %d, |1> = %d", "Last %zu: |0> = %d, |1> = %d", "最近 %zu 个: |0> = %d, |1> = %d", "Последние %zu: |0> = %d, |1> = %d", "%zu derniers : |0> = %d, |1> = %d", "Letzte %zu: |0> = %d, |1> = %d"},

        // ── Transmitter（量子训练器 GUI）────────────
        {"File", "File", "文件", "Файл", "Fichier", "Datei"},
        {"New", "New", "新建", "Создать", "Nouveau", "Neu"},
        {"Open...", "Open...", "打开...", "Открыть...", "Ouvrir...", "Öffnen..."},
        {"Save", "Save", "保存", "Сохранить", "Enregistrer", "Speichern"},
        {"Export", "Export", "导出", "Экспорт", "Exporter", "Exportieren"},
        {"Recent Files", "Recent Files", "最近文件", "Недавние файлы", "Fichiers récents", "Zuletzt verwendet"},
        {"Edit", "Edit", "编辑", "Правка", "Édition", "Bearbeiten"},
        {"Select All", "Select All", "全选", "Выбрать всё", "Tout sélectionner", "Alle auswählen"},
        {"Deselect All", "Deselect All", "取消全选", "Снять выделение", "Tout désélectionner", "Auswahl aufheben"},
        {"Invert Selection", "Invert Selection", "反选", "Инвертировать выделение", "Inverser la sélection", "Auswahl umkehren"},
        {"View", "View", "视图", "Вид", "Affichage", "Ansicht"},
        {"Refresh", "Refresh", "刷新", "Обновить", "Actualiser", "Aktualisieren"},
        {"Toggle Left", "Toggle Left", "切换左侧", "Переключить слева", "Basculer à gauche", "Links umschalten"},
        {"Toggle Right", "Toggle Right", "切换右侧", "Переключить справа", "Basculer à droite", "Rechts umschalten"},
        {"Training", "Training", "训练", "Обучение", "Entraînement", "Training"},
        {"Start Training", "Start Training", "开始训练", "Начать обучение", "Démarrer l'entraînement", "Training starten"},
        {"Stop Training", "Stop Training", "停止训练", "Остановить обучение", "Arrêter l'entraînement", "Training stoppen"},
        {"Configure...", "Configure...", "配置...", "Настроить...", "Configurer...", "Konfigurieren..."},
        {"Network", "Network", "网络", "Сеть", "Réseau", "Netzwerk"},
        {"Local Mode (LM)", "Local Mode (LM)", "本地模式 (LM)", "Локальный режим (LM)", "Mode local (LM)", "Lokaler Modus (LM)"},
        {"Shared Mode (SM)", "Shared Mode (SM)", "共享模式 (SM)", "Общий режим (SM)", "Mode partagé (SM)", "Gemeinsamer Modus (SM)"},
        {"Quantum Net (QNM)", "Quantum Net (QNM)", "量子网络 (QNM)", "Квантовая сеть (QNM)", "Réseau quantique (QNM)", "Quantennetz (QNM)"},
        {"Help", "Help", "帮助", "Помощь", "Aide", "Hilfe"},
        {"About QQNT", "About QQNT", "关于 QQNT", "О QQNT", "À propos de QQNT", "Über QQNT"},
        {"Documentation", "Documentation", "文档", "Документация", "Documentation", "Dokumentation"},
        {"Keyboard Shortcuts", "Keyboard Shortcuts", "键盘快捷键", "Сочетания клавиш", "Raccourcis clavier", "Tastenkürzel"},
        {"Drive / Directory", "Drive / Directory", "驱动器 / 目录", "Диск / каталог", "Lecteur / répertoire", "Laufwerk / Verzeichnis"},
        {"Up", "Up", "上级", "Вверх", "Haut", "Nach oben"},
        {"Deselect", "Deselect", "取消选择", "Отменить выбор", "Désélectionner", "Abwählen"},
        {"Training Data", "Training Data", "训练数据", "Данные обучения", "Données d'entraînement", "Trainingsdaten"},
        {"Path: %s", "Path: %s", "路径: %s", "Путь: %s", "Chemin : %s", "Pfad: %s"},
        {"Clear", "Clear", "清空", "Очистить", "Effacer", "Leeren"},
        {"Training Parameters", "Training Parameters", "训练参数", "Параметры обучения", "Paramètres d'entraînement", "Trainingsparameter"},
        {"Model:", "Model:", "模型:", "Модель:", "Modèle :", "Modell:"},
        {"Epochs:", "Epochs:", "轮数:", "Эпохи:", "Époques :", "Epochen:"},
        {"Learn Rate:", "Learn Rate:", "学习率:", "Скорость обучения:", "Taux d'apprentissage :", "Lernrate:"},
        {"Qubits:", "Qubits:", "量子比特:", "Кубиты:", "Qubits :", "Qubits:"},
        {"Layers:", "Layers:", "层数:", "Слои:", "Couches :", "Schichten:"},
        {"Output:", "Output:", "输出:", "Вывод:", "Sortie :", "Ausgabe:"},
        {"Save Config", "Save Config", "保存配置", "Сохранить конфигурацию", "Enregistrer la config", "Konfiguration speichern"},
        {"Actions", "Actions", "操作", "Действия", "Actions", "Aktionen"},
        {"Delete", "Delete", "删除", "Удалить", "Supprimer", "Löschen"},
        {"Mode:", "Mode:", "模式:", "Режим:", "Mode :", "Modus:"},
        {"LM (Local Mode)", "LM (Local Mode)", "LM (本地模式)", "LM (локальный)", "LM (local)", "LM (lokal)"},
        {"SM (Shared Mode)", "SM (Shared Mode)", "SM (共享模式)", "SM (общий)", "SM (partagé)", "SM (gemeinsam)"},
        {"QNM (Quantum Network Mode)", "QNM (Quantum Network Mode)", "QNM (量子网络模式)", "QNM (квантовая сеть)", "QNM (réseau quantique)", "QNM (Quantennetz)"},
        {"Active Nodes: %zu", "Active Nodes: %zu", "活动节点: %zu", "Активные узлы: %zu", "Nœuds actifs : %zu", "Aktive Knoten: %zu"},
        {"Status: Scanning...", "Status: Scanning...", "状态: 扫描中...", "Статус: сканирование...", "Statut : analyse...", "Status: Scanne..."},
        {"Status: Ready", "Status: Ready", "状态: 就绪", "Статус: готово", "Statut : prêt", "Status: Bereit"},
        {"Local drive mode — read/write from local filesystem", "Local drive mode — read/write from local filesystem", "本地磁盘模式 — 从本地文件系统读写", "Локальный режим — чтение/запись из локальной ФС", "Mode disque local — lecture/écriture depuis le système de fichiers", "Lokaler Modus — Lesen/Schreiben vom lokalen Dateisystem"},
    };

    const LangEntry kLangEntries[] = {
        {Lang::EN, "English"},
        {Lang::ZH, "中文"},
        {Lang::RU, "Русский"},
        {Lang::FR, "Français"},
        {Lang::DE, "Deutsch"},
    };

    Lang g_lang = Lang::EN;

    const char *entry_text(const Entry &e, Lang l)
    {
        switch (l)
        {
        case Lang::EN: return e.en;
        case Lang::ZH: return e.zh;
        case Lang::RU: return e.ru;
        case Lang::FR: return e.fr;
        case Lang::DE: return e.de;
        }
        return e.en;
    }

    std::string lang_store_path()
    {
    #ifdef _WIN32
        if (const char *appdata = std::getenv("APPDATA"))
            return std::string(appdata) + "/ravendrel/locale";
        return "ravendrel.locale";
    #else
        if (const char *home = std::getenv("HOME"))
            return std::string(home) + "/.ravendrel/locale";
        return ".ravendrel/locale";
    #endif
    }

    Lang lang_from_code(const std::string &code)
    {
        if (code == "zh") return Lang::ZH;
        if (code == "ru") return Lang::RU;
        if (code == "fr") return Lang::FR;
        if (code == "de") return Lang::DE;
        return Lang::EN;
    }

    bool read_saved_lang(Lang &out)
    {
        std::ifstream in(lang_store_path());
        if (!in)
            return false;
        std::string code;
        in >> code;
        if (code.empty())
            return false;
        out = lang_from_code(code);
        return true;
    }

    void write_saved_lang(Lang l)
    {
        try
        {
            std::filesystem::path p = lang_store_path();
            std::filesystem::create_directories(p.parent_path());
            std::ofstream out(p);
            if (out)
                out << lang_code(l) << "\n";
        }
        catch (...)
        {
        }
    }

    Lang lang_from_locale_name(const char *name)
    {
        if (!name)
            return Lang::EN;
        std::string n;
        for (const char *p = name; *p; ++p)
            n += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        if (n.rfind("zh", 0) == 0 || n.find("chinese") != std::string::npos) return Lang::ZH;
        if (n.rfind("ru", 0) == 0 || n.find("russian") != std::string::npos) return Lang::RU;
        if (n.rfind("fr", 0) == 0 || n.find("french") != std::string::npos) return Lang::FR;
        if (n.rfind("de", 0) == 0 || n.find("german") != std::string::npos) return Lang::DE;
        return Lang::EN;
    }

    }

    Lang current_lang()
    {
        return g_lang;
    }

    void set_lang(Lang l)
    {
        g_lang = l;
        write_saved_lang(l);
    }

    Lang detect_lang()
    {
        Lang saved;
        if (read_saved_lang(saved))
            return saved;
        if (const char *env = std::getenv("LC_ALL"))
            return lang_from_locale_name(env);
        if (const char *env = std::getenv("LC_MESSAGES"))
            return lang_from_locale_name(env);
        if (const char *env = std::getenv("LANG"))
            return lang_from_locale_name(env);
        try
        {
            std::string name = std::locale("").name();
            return lang_from_locale_name(name.c_str());
        }
        catch (...)
        {
        }
        return Lang::EN;
    }

    const char *tr(const char *key)
    {
        for (const auto &e : kEntries)
        {
            if (std::strcmp(e.key, key) == 0)
                return entry_text(e, g_lang);
        }
        return key;
    }

    const LangEntry *available_langs()
    {
        return kLangEntries;
    }

    int available_lang_count()
    {
        return static_cast<int>(sizeof(kLangEntries) / sizeof(kLangEntries[0]));
    }

    const char *lang_code(Lang l)
    {
        switch (l)
        {
        case Lang::EN: return "en";
        case Lang::ZH: return "zh";
        case Lang::RU: return "ru";
        case Lang::FR: return "fr";
        case Lang::DE: return "de";
        }
        return "en";
    }
}