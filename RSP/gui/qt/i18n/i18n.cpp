#include "i18n.h"

#include <QSettings>
#include <QLocale>
#include <QVariant>

namespace rvdlrsp::gui
{

    namespace
    {

        const QStringList kLanguages = {"en", "zh", "ru", "fr", "de"};

        QString native_name(const QString &code)
        {
            if (code == "en") return QStringLiteral("English");
            if (code == "zh") return QStringLiteral("中文");
            if (code == "ru") return QStringLiteral("Русский");
            if (code == "fr") return QStringLiteral("Français");
            if (code == "de") return QStringLiteral("Deutsch");
            return code;
        }

        QString detect_from_system()
        {
            const QString name = QLocale::system().name().toLower(); // 例如 "zh_cn"、"ru_ru"
            if (name.startsWith("zh")) return "zh";
            if (name.startsWith("ru")) return "ru";
            if (name.startsWith("fr")) return "fr";
            if (name.startsWith("de")) return "de";
            return "en";
        }
    }

    I18n &I18n::instance()
    {
        static I18n inst;
        return inst;
    }

    const QStringList &I18n::languages()
    {
        return kLanguages;
    }

    QString I18n::languageName(const QString &code)
    {
        return native_name(code);
    }

    QString I18n::current() const
    {
        return current_;
    }

    void I18n::setLanguage(const QString &code)
    {
        if (!kLanguages.contains(code))
            return;
        current_ = code;
        QSettings().setValue("language", code);
    }

    QString I18n::tr(const QString &source) const
    {
        auto lang_it = dict_.constFind(source);
        if (lang_it == dict_.constEnd())
            return source;
        auto it = lang_it->constFind(current_);
        if (it == lang_it->constEnd())
            return source;
        return it.value();
    }

    void I18n::load_saved()
    {
        const QVariant saved = QSettings().value("language");
        if (saved.isValid() && kLanguages.contains(saved.toString()))
        {
            current_ = saved.toString();
            return;
        }
        current_ = detect_from_system();
    }

    void I18n::add(const char *zh, const char *en, const char *ru, const char *fr, const char *de)
    {
        QHash<QString, QString> m;
        m.insert("zh", QString::fromUtf8(zh));
        m.insert("en", QString::fromUtf8(en));
        m.insert("ru", QString::fromUtf8(ru));
        m.insert("fr", QString::fromUtf8(fr));
        m.insert("de", QString::fromUtf8(de));
        dict_.insert(QString::fromUtf8(zh), m);
    }

    I18n::I18n() : current_("zh")
    {
        // ─── 主窗口 / 菜单栏 / 工具栏 ──────────────────────────────
        add("Ravendrel RSP — 量子机器人仿真平台", "Ravendrel RSP — Quantum Robot Simulation Platform", "Ravendrel RSP — платформа симуляции квантовых роботов", "Ravendrel RSP — Plateforme de simulation de robots quantiques", "Ravendrel RSP — Quantenroboter-Simulationsplattform");
        add("文件(&F)", "&File", "&Файл", "&Fichier", "&Datei");
        add("退出(&Q)", "E&xit", "Вы&ход", "&Quitter", "&Beenden");
        add("仿真(&S)", "&Simulation", "&Симуляция", "&Simulation", "&Simulation");
        add("启动遥操作", "Start Teleop", "Запустить телеоперацию", "Démarrer la téléopération", "Teleoperation starten");
        add("停止遥操作", "Stop Teleop", "Остановить телеоперацию", "Arrêter la téléopération", "Teleoperation stoppen");
        add("暂停物理", "Pause Physics", "Пауза физики", "Suspendre la physique", "Physik pausieren");
        add("单步", "Step Once", "Один шаг", "Pas à pas", "Einzelschritt");
        add("视图(&V)", "&View", "&Вид", "&Affichage", "&Ansicht");
        add("主工具栏", "Main Toolbar", "Главная панель", "Barre d'outils principale", "Hauptsymbolleiste");
        add("▶ 遥操作", "▶ Teleop", "▶ Телеоперация", "▶ Téléopération", "▶ Teleoperation");
        add("⏸ 暂停", "⏸ Pause", "⏸ Пауза", "⏸ Pause", "⏸ Pause");
        add("⏭ 单步", "⏭ Step", "⏭ Шаг", "⏭ Pas", "⏭ Schritt");
        add("场景层级", "Scene Hierarchy", "Иерархия сцены", "Hiérarchie de scène", "Szenenhierarchie");
        add("资产属性", "Asset Details", "Свойства актива", "Détails de l'actif", "Asset-Details");
        add("实体属性", "Entity Details", "Свойства сущности", "Détails de l'entité", "Entitätsdetails");
        add("遥操作", "Teleop", "Телеоперация", "Téléopération", "Teleoperation");
        add("物理", "Physics", "Физика", "Physique", "Physik");
        add("强化学习", "Reinforcement Learning", "Обучение с подкреплением", "Apprentissage par renforcement", "Verstärkendes Lernen");
        add("量子", "Quantum", "Квант", "Quantique", "Quanten");
        add("意识", "Consciousness", "Сознание", "Conscience", "Bewusstsein");
        add("脑机桥", "Brain Bridge", "Нейроинтерфейс", "Interface cerveau", "Gehirn-Brücke");
        add("电路", "Circuit", "Схема", "Circuit", "Schaltkreis");
        add("蓝图", "Blueprint", "Блюпринт", "Blueprint", "Blueprint");
        add("场景图", "Scene Graph", "Граф сцены", "Graphe de scène", "Szenengraph");
        add("日志", "Log", "Журнал", "Journal", "Protokoll");
        add("指标", "Metrics", "Метрики", "Métriques", "Metriken");
        add("分形地形", "Fractal Terrain", "Фрактальный рельеф", "Terrain fractal", "Fraktales Gelände");
        add("内容浏览器", "Content Browser", "Браузер контента", "Navigateur de contenu", "Inhaltsbrowser");
        add("FPS: %1", "FPS: %1", "FPS: %1", "FPS : %1", "FPS: %1");
        add("步数: %1", "Steps: %1", "Шаги: %1", "Étapes : %1", "Schritte: %1");
        add("后端: %1", "Backend: %1", "Бэкенд: %1", "Backend : %1", "Backend: %1");
        add("GPU: 未启用", "GPU: Disabled", "GPU: отключено", "GPU : désactivé", "GPU: deaktiviert");
        add("GPU: 已启用", "GPU: Enabled", "GPU: включено", "GPU : activé", "GPU: aktiviert");
        add("GPU: 已勾选但后端未启用", "GPU: selected but backend disabled", "GPU: выбрано, но бэкенд отключён", "GPU : sélectionné mais backend désactivé", "GPU: ausgewählt, aber Backend deaktiviert");
        add("重命名", "Rename", "Переименовать", "Renommer", "Umbenennen");
        add("重命名实体", "Rename Entity", "Переименовать сущность", "Renommer l'entité", "Entität umbenennen");
        add("名称：", "Name:", "Имя:", "Nom :", "Name:");
        add("删除实体", "Delete Entity", "Удалить сущность", "Supprimer l'entité", "Entität löschen");
        add("语言(&L)", "&Language", "&Язык", "&Langue", "&Sprache");
        add("工具(&T)", "&Tools", "&Инструменты", "&Outils", "&Werkzeuge");
        add("设置(&S)", "&Settings", "&Настройки", "&Paramètres", "&Einstellungen");
        add("设置", "Settings", "Настройки", "Paramètres", "Einstellungen");
        add("主题", "Theme", "Тема", "Thème", "Design");
        add("语言", "Language", "Язык", "Langue", "Sprache");
        add("日志级别", "Log Level", "Уровень журнала", "Niveau de journal", "Protokollstufe");
        add("World Outliner", "World Outliner", "Иерархия мира", "Explorateur de monde", "Welt-Outliner");
        add("Details", "Details", "Свойства", "Détails", "Details");
        add("Content Browser", "Content Browser", "Браузер контента", "Navigateur de contenu", "Inhaltsbrowser");
        add("切换白天/夜间模式", "Toggle Day/Night Mode", "Переключить режим день/ночь", "Basculer le mode jour/nuit", "Tag-/Nachtmodus umschalten");
        add("夜间模式", "Night Mode", "Ночной режим", "Mode nuit", "Nachtmodus");
        add("白天模式", "Day Mode", "Дневной режим", "Mode jour", "Tagmodus");

        // ─── 面板（panels.cpp）────────────────────────────────────
        add("遥操作 Teleop", "Teleop", "Телеоперация", "Téléopération", "Teleoperation");
        add("关节角 (rad):", "Joint Angle (rad):", "Угол сустава (рад):", "Angle d'articulation (rad) :", "Gelenkwinkel (rad):");
        add("空闲", "Idle", "Простой", "Inactif", "Leerlauf");
        add("停止", "Stop", "Стоп", "Arrêter", "Stopp");
        add("物理引擎 Physics", "Physics Engine", "Физический движок", "Moteur physique", "Physik-Engine");
        add("重力 g (m/s²):", "Gravity g (m/s²):", "Гравитация g (м/с²):", "Gravité g (m/s²) :", "Schwerkraft g (m/s²):");
        add("求解迭代次数:", "Solver Iterations:", "Итерации решателя:", "Itérations du solveur :", "Löser-Iterationen:");
        add("运行中", "Running", "Выполняется", "En cours", "Läuft");
        add("暂停", "Pause", "Пауза", "Pause", "Pause");
        add("强化学习 RL", "Reinforcement Learning", "Обучение с подкреплением", "Apprentissage par renforcement", "Verstärkendes Lernen");
        add("Reward: %1", "Reward: %1", "Награда: %1", "Récompense : %1", "Belohnung: %1");
        add("Episode: %1", "Episode: %1", "Эпизод: %1", "Épisode : %1", "Episode: %1");
        add("开始训练", "Start Training", "Начать обучение", "Démarrer l'entraînement", "Training starten");
        add("量子后端 Quantum", "Quantum Backend", "Квантовый бэкенд", "Backend quantique", "Quanten-Backend");
        add("量子比特数: %1", "Qubits: %1", "Кубиты: %1", "Qubits : %1", "Qubits: %1");
        add("意识控制器 Consciousness", "Consciousness Controller", "Контроллер сознания", "Contrôleur de conscience", "Bewusstseinssteuerung");
        add("脑机桥 BrainBridge", "BrainBridge", "Нейроинтерфейс", "BrainBridge", "BrainBridge");
        add("机器人电路 Circuit", "Robot Circuit", "Схема робота", "Circuit du robot", "Roboter-Schaltkreis");
        add("蓝图 Blueprint", "Blueprint", "Блюпринт", "Blueprint", "Blueprint");
        add("场景图 Scene Graph", "Scene Graph", "Граф сцены", "Graphe de scène", "Szenengraph");
        add("日志 Log", "Log", "Журнал", "Journal", "Protokoll");
        add("指标 Metrics", "Metrics", "Метрики", "Métriques", "Metriken");
        add("帧时间: %1 ms", "Frame Time: %1 ms", "Время кадра: %1 мс", "Temps de trame : %1 ms", "Frame-Zeit: %1 ms");
        add("仿真步数: %1", "Sim Steps: %1", "Шаги симуляции: %1", "Étapes de sim : %1", "Sim-Schritte: %1");
        add("态矢量残差 |1-‖ψ‖|: %1", "State Vector Residual |1-‖ψ‖|: %1", "Невязка вектора состояния |1-‖ψ‖|: %1", "Résidu du vecteur d'état |1-‖ψ‖| : %1", "Zustandsvektor-Residuum |1-‖ψ‖|: %1");
        add("分形地形 Fractal Terrain", "Fractal Terrain", "Фрактальный рельеф", "Terrain fractal", "Fraktales Gelände");
        add("启用分形地形", "Enable Fractal Terrain", "Включить фрактальный рельеф", "Activer le terrain fractal", "Fraktales Gelände aktivieren");
        add("分辨率（每边采样点）:", "Resolution (samples per side):", "Разрешение (отсчётов на сторону):", "Résolution (échantillons par côté) :", "Auflösung (Abtastpunkte pro Seite):");
        add("范围 extent（截面 [-e, e]²）:", "Extent (slice [-e, e]²):", "Протяжённость (сечение [-e, e]²):", "Étendue (tranche [-e, e]²) :", "Ausdehnung (Schnitt [-e, e]²):");
        add("高度系数 height_scale:", "Height Scale:", "Масштаб высоты:", "Échelle de hauteur :", "Höhenskala:");
        add("最大迭代 max_iter:", "Max Iterations:", "Макс. итераций:", "Itérations max :", "Max. Iterationen:");
        add("固定 s 分量 slice_s:", "Fixed s Component slice_s:", "Фиксированная компонента s:", "Composante s fixe :", "Feste s-Komponente:");
        add("应用并重新生成", "Apply & Regenerate", "Применить и пересоздать", "Appliquer et régénérer", "Anwenden & neu generieren");
        add("实体属性 Entity Details", "Entity Details", "Свойства сущности", "Détails de l'entité", "Entitätsdetails");
        add("未选中实体", "No Entity Selected", "Сущность не выбрана", "Aucune entité sélectionnée", "Keine Entität ausgewählt");
        add("位置 Position (m):", "Position (m):", "Позиция (м):", "Position (m) :", "Position (m):");
        add("旋转 Rotation (°):", "Rotation (°):", "Вращение (°):", "Rotation (°) :", "Rotation (°):");
        add("缩放 Scale:", "Scale:", "Масштаб:", "Échelle :", "Skalierung:");
        add("类型: %1", "Type: %1", "Тип: %1", "Type : %1", "Typ: %1");
        add("质量: %1 kg", "Mass: %1 kg", "Масса: %1 кг", "Masse : %1 kg", "Masse: %1 kg");
        add("碰撞体: %1", "Collider: %1", "Коллайдер: %1", "Collider : %1", "Kollider: %1");
        add("资产属性 Asset Details", "Asset Details", "Свойства актива", "Détails de l'actif", "Asset-Details");
        add("未选中资产", "No Asset Selected", "Актив не выбран", "Aucun actif sélectionné", "Kein Asset ausgewählt");
        add("保存", "Save", "Сохранить", "Enregistrer", "Speichern");
        add("放弃修改", "Discard Changes", "Отменить изменения", "Annuler les modifications", "Änderungen verwerfen");
        add("撤销", "Undo", "Отменить", "Annuler", "Rückgängig");
        add("重做", "Redo", "Повторить", "Rétablir", "Wiederholen");
        add("撤销栈: %1  重做栈: %2", "Undo: %1  Redo: %2", "Отмена: %1  Повтор: %2", "Annuler : %1  Rétablir : %2", "Rückgängig: %1  Wiederholen: %2");
        add("Tab 缩进(空格)", "Tab Indent (spaces)", "Отступ Tab (пробелы)", "Indentation Tab (espaces)", "Tab-Einzug (Leerzeichen)");
        add("自动换行", "Word Wrap", "Перенос строк", "Retour à la ligne", "Zeilenumbruch");
        add("路径: %1", "Path: %1", "Путь: %1", "Chemin : %1", "Pfad: %1");
        add("大小: %1 字节", "Size: %1 bytes", "Размер: %1 байт", "Taille : %1 octets", "Größe: %1 Bytes");
        add("大小: %1 字节   修改时间: %2", "Size: %1 bytes   Modified: %2", "Размер: %1 байт   Изменён: %2", "Taille : %1 octets   Modifié : %2", "Größe: %1 Bytes   Geändert: %2");
        add("*已修改", "*Modified", "*Изменено", "*Modifié", "*Geändert");
        add("✓已检出", "✓Checked Out", "✓Извлечено", "✓Extrait", "✓Ausgecheckt");
        add("+新增", "+Added", "+Добавлено", "+Ajouté", "+Hinzugefügt");
        add("干净", "Clean", "Чисто", "Propre", "Sauber");
        add("状态: %1", "Status: %1", "Статус: %1", "Statut : %1", "Status: %1");
        add("（二进制资产，不支持文本编辑）", "(Binary asset, text editing not supported)", "(Бинарный актив, редактирование текста не поддерживается)", "(Actif binaire, édition de texte non prise en charge)", "(Binäres Asset, Textbearbeitung nicht unterstützt)");
        add("（无法读取文件）", "(Cannot read file)", "(Не удалось прочитать файл)", "(Impossible de lire le fichier)", "(Datei kann nicht gelesen werden)");

        // ─── 内容浏览器（content_browser.cpp）────────────────────
        add("文件夹", "Folder", "Папка", "Dossier", "Ordner");
        add("机器人", "Robot", "Робот", "Robot", "Roboter");
        add("模型", "Model", "Модель", "Modèle", "Modell");
        add("材质", "Material", "Материал", "Matériau", "Material");
        add("场景", "Scene", "Сцена", "Scène", "Szene");
        add("Rvdl 脚本", "Rvdl Script", "Скрипт Rvdl", "Script Rvdl", "Rvdl-Skript");
        add("音频", "Audio", "Аудио", "Audio", "Audio");
        add("纹理", "Texture", "Текстура", "Texture", "Textur");
        add("资产", "Asset", "Актив", "Actif", "Asset");
        add("取消缩略图渲染", "Cancel Thumbnail Rendering", "Отменить рендеринг миниатюр", "Annuler le rendu des vignettes", "Vorschaurendering abbrechen");
        add("过滤资产…", "Filter assets…", "Фильтр активов…", "Filtrer les actifs…", "Assets filtern…");
        add("缩略图 %1 / %2 (%3%)", "Thumbnails %1 / %2 (%3%)", "Миниатюры %1 / %2 (%3%)", "Vignettes %1 / %2 (%3%)", "Vorschaubilder %1 / %2 (%3%)");
        add("打开", "Open", "Открыть", "Ouvrir", "Öffnen");
        add("新建", "New", "Создать", "Nouveau", "Neu");
        add("新建文件夹", "New Folder", "Новая папка", "Nouveau dossier", "Neuer Ordner");
        add("Rvdl 脚本 (.rvdl)", "Rvdl Script (.rvdl)", "Скрипт Rvdl (.rvdl)", "Script Rvdl (.rvdl)", "Rvdl-Skript (.rvdl)");
        add("场景 (.qscene)", "Scene (.qscene)", "Сцена (.qscene)", "Scène (.qscene)", "Szene (.qscene)");
        add("蓝图 (.qbp)", "Blueprint (.qbp)", "Блюпринт (.qbp)", "Blueprint (.qbp)", "Blueprint (.qbp)");
        add("材质 (.qmat)", "Material (.qmat)", "Материал (.qmat)", "Matériau (.qmat)", "Material (.qmat)");
        add("机器人 (.qrobot)", "Robot (.qrobot)", "Робот (.qrobot)", "Robot (.qrobot)", "Roboter (.qrobot)");
        add("文本 (.txt)", "Text (.txt)", "Текст (.txt)", "Texte (.txt)", "Text (.txt)");
        add("粘贴", "Paste", "Вставить", "Coller", "Einfügen");
        add("查看", "View", "Вид", "Affichage", "Ansicht");
        add("超大图标", "Extra Large Icons", "Очень крупные значки", "Très grandes icônes", "Sehr große Symbole");
        add("大图标", "Large Icons", "Крупные значки", "Grandes icônes", "Große Symbole");
        add("列表", "List", "Список", "Liste", "Liste");
        add("详细信息", "Details", "Подробности", "Détails", "Details");
        add("刷新", "Refresh", "Обновить", "Actualiser", "Aktualisieren");
        add("新建资产", "New Asset", "Новый актив", "Nouvel actif", "Neues Asset");
        add("粘贴文本.txt", "pasted text.txt", "вставленный текст.txt", "texte collé.txt", "eingefügter Text.txt");

        // ─── 工程浏览器（project_browser.cpp）────────────────────
        add("Ravendrel RSP — 工程浏览器", "Ravendrel RSP — Project Browser", "Ravendrel RSP — браузер проектов", "Ravendrel RSP — Navigateur de projets", "Ravendrel RSP — Projektbrowser");
        add("<b>历史仿真任务</b>", "<b>History</b>", "<b>История</b>", "<b>Historique</b>", "<b>Verlauf</b>");
        add("删除", "Delete", "Удалить", "Supprimer", "Löschen");
        add("归档", "Archive", "Архивировать", "Archiver", "Archivieren");
        add("项目信息", "Project Info", "Информация о проекте", "Infos du projet", "Projektinfo");
        add("任务名称", "Task Name", "Имя задачи", "Nom de la tâche", "Aufgabenname");
        add("机器人类型", "Robot Type", "Тип робота", "Type de robot", "Robotertyp");
        add("机器臂", "Robot Arm", "Рука-манипулятор", "Bras robotique", "Roboterarm");
        add("移动机器人", "Mobile Robot", "Мобильный робот", "Robot mobile", "Mobiler Roboter");
        add("无人机", "Drone", "Дрон", "Drone", "Drohne");
        add("人形机器人", "Humanoid", "Гуманоид", "Humanoïde", "Humanoid");
        add("自定义", "Custom", "Пользовательский", "Personnalisé", "Benutzerdefiniert");
        add("仿真场景模板", "Scene Template", "Шаблон сцены", "Modèle de scène", "Szenenvorlage");
        add("参数设置", "Parameters", "Параметры", "Paramètres", "Parameter");
        add("启用 GPU 加速", "Enable GPU Acceleration", "Включить ускорение GPU", "Activer l'accélération GPU", "GPU-Beschleunigung aktivieren");
        add("仿真引擎版本", "Engine Version", "Версия движка", "Version du moteur", "Engine-Version");
        add("物理引擎", "Physics Engine", "Физический движок", "Moteur physique", "Physik-Engine");
        add("仿真后端", "Simulation Backend", "Бэкенд симуляции", "Backend de simulation", "Simulations-Backend");
        add("工作区切换", "Workspace", "Рабочая область", "Espace de travail", "Arbeitsbereich");
        add("仿真调试", "Simulation Debug", "Отладка симуляции", "Débogage de simulation", "Simulations-Debug");
        add("机器人控制", "Robot Control", "Управление роботом", "Contrôle du robot", "Robotersteuerung");
        add("场景编辑", "Scene Editing", "Редактирование сцены", "Édition de scène", "Szenenbearbeitung");
        add("启动仿真 →", "Launch Simulation →", "Запустить симуляцию →", "Lancer la simulation →", "Simulation starten →");
        add("删除项目", "Delete Project", "Удалить проект", "Supprimer le projet", "Projekt löschen");
        add("确定删除项目 %1 吗？此操作不可撤销。", "Delete project %1? This cannot be undone.", "Удалить проект %1? Это действие необратимо.", "Supprimer le projet %1 ? Cette action est irréversible.", "Projekt %1 löschen? Dies kann nicht rückgängig gemacht werden.");
        add("删除失败：%1", "Delete failed: %1", "Не удалось удалить: %1", "Échec de la suppression : %1", "Löschen fehlgeschlagen: %1");
        add("归档项目", "Archive Project", "Архивировать проект", "Archiver le projet", "Projekt archivieren");
        add("归档失败：%1", "Archive failed: %1", "Не удалось архивировать: %1", "Échec de l'archivage : %1", "Archivierung fehlgeschlagen: %1");

        // ─── 加载界面（loading_screen.cpp）───────────────────────
        add("正在启动仿真任务", "Starting Simulation Task", "Запуск задачи симуляции", "Démarrage de la tâche de simulation", "Simulationsaufgabe wird gestartet");
        add("Ravendrel RSP — 正在启动仿真任务", "Ravendrel RSP — Starting Simulation Task", "Ravendrel RSP — запуск задачи симуляции", "Ravendrel RSP — Démarrage de la tâche de simulation", "Ravendrel RSP — Simulationsaufgabe wird gestartet");
        add("项目：%1", "Project: %1", "Проект: %1", "Projet : %1", "Projekt: %1");
        add("加载机器人硬件/模拟模型", "Loading robot hardware / simulation model", "Загрузка аппаратной / имитационной модели робота", "Chargement du modèle matériel / de simulation du robot", "Roboter-Hardware / Simulationsmodell wird geladen");
        add("初始化物理引擎", "Initializing physics engine", "Инициализация физического движка", "Initialisation du moteur physique", "Physik-Engine wird initialisiert");
        add("加载仿真场景环境（地形/障碍物/灯光/温度/风速）", "Loading scene environment (terrain / obstacles / lighting / temperature / wind)", "Загрузка окружения сцены (рельеф / препятствия / свет / температура / ветер)", "Chargement de l'environnement de scène (terrain / obstacles / éclairage / température / vent)", "Szenenumgebung wird geladen (Gelände / Hindernisse / Licht / Temperatur / Wind)");
        add("初始化量子设备并尝试连接", "Initializing quantum device and connecting", "Инициализация квантового устройства и подключение", "Initialisation de l'appareil quantique et connexion", "Quantengerät wird initialisiert und verbunden");
        add("初始化脑量子接口", "Initializing brain-quantum interface", "Инициализация нейро-квантового интерфейса", "Initialisation de l'interface cerveau-quantique", "Gehirn-Quanten-Schnittstelle wird initialisiert");
        add("初始化量子学习机", "Initializing quantum learning machine", "Инициализация квантовой обучающей машины", "Initialisation de la machine d'apprentissage quantique", "Quanten-Lernmaschine wird initialisiert");
        add("初始化机器人控制接口", "Initializing robot control interface", "Инициализация интерфейса управления роботом", "Initialisation de l'interface de contrôle du robot", "Robotersteuerungsschnittstelle wird initialisiert");
        add("进入正式仿真主面", "Entering simulation main view", "Переход в основной вид симуляции", "Entrée dans la vue principale de simulation", "Hauptansicht der Simulation wird geöffnet");
        add("初始化失败", "Initialization failed", "Ошибка инициализации", "Échec de l'initialisation", "Initialisierung fehlgeschlagen");

        // ─── 脚本编辑器（script_editor.cpp）──────────────────────
        add("rvdl 脚本编辑器 — %1", "rvdl Script Editor — %1", "Редактор скриптов rvdl — %1", "Éditeur de script rvdl — %1", "rvdl-Skript-Editor — %1");
        add("查找/替换", "Find/Replace", "Найти/Заменить", "Rechercher/Remplacer", "Suchen/Ersetzen");
        add("查找…", "Find...", "Найти...", "Rechercher...", "Suchen...");
        add("替换为…", "Replace with...", "Заменить на...", "Remplacer par...", "Ersetzen durch...");
        add("下一个", "Next", "Далее", "Suivant", "Weiter");
        add("上一个", "Previous", "Назад", "Précédent", "Zurück");
        add("替换", "Replace", "Заменить", "Remplacer", "Ersetzen");
        add("全部替换", "Replace All", "Заменить все", "Tout remplacer", "Alle ersetzen");
        add("区分大小写", "Match Case", "С учётом регистра", "Respecter la casse", "Groß-/Kleinschreibung");
        add("未找到匹配项。", "No matches found.", "Совпадений не найдено.", "Aucune correspondance trouvée.", "Keine Übereinstimmungen gefunden.");
        add("▶ 运行", "▶ Run", "▶ Выполнить", "▶ Exécuter", "▶ Ausführen");
        add("保存 (Ctrl+S)", "Save (Ctrl+S)", "Сохранить (Ctrl+S)", "Enregistrer (Ctrl+S)", "Speichern (Strg+S)");
        add("关闭", "Close", "Закрыть", "Fermer", "Schließen");
        add("运行输出将显示在这里…", "Run output will appear here…", "Вывод выполнения появится здесь…", "La sortie d'exécution apparaîtra ici…", "Ausgabe wird hier angezeigt…");
        add("已保存", "Saved", "Сохранено", "Enregistré", "Gespeichert");
        add("[运行] %1", "[Run] %1", "[Выполнение] %1", "[Exécution] %1", "[Ausführen] %1");
        add("[错误] Ravendrel Daemon 未运行（端口 50052）。", "[Error] Ravendrel Daemon is not running (port 50052).", "[Ошибка] Демон Ravendrel не запущен (порт 50052).", "[Erreur] Le démon Ravendrel n'est pas démarré (port 50052).", "[Fehler] Ravendrel-Daemon läuft nicht (Port 50052).");
        add("       请先启动: runtime --daemon", "       Please start: runtime --daemon", "       Запустите: runtime --daemon", "       Démarrez d'abord : runtime --daemon", "       Bitte starten: runtime --daemon");
        add("       然后再点击运行。", "       Then click run again.", "       Затем снова нажмите выполнить.", "       Puis cliquez à nouveau sur exécuter.", "       Klicken Sie dann erneut auf Ausführen.");
        add("[错误] 未找到 rvdl.cmd 解释器", "[Error] rvdl.cmd interpreter not found", "[Ошибка] Интерпретатор rvdl.cmd не найден", "[Erreur] Interpréteur rvdl.cmd introuvable", "[Fehler] rvdl.cmd-Interpreter nicht gefunden");
        add("[完成] 退出码 %1", "[Done] Exit code %1", "[Готово] Код выхода %1", "[Terminé] Code de sortie %1", "[Fertig] Exit-Code %1");
        add("保存失败", "Save failed", "Не удалось сохранить", "Échec de l'enregistrement", "Speichern fehlgeschlagen");
        add("无法写入文件：\n%1", "Cannot write file:\n%1", "Не удалось записать файл:\n%1", "Impossible d'écrire le fichier :\n%1", "Datei kann nicht geschrieben werden:\n%1");
        add("未保存的更改", "Unsaved Changes", "Несохранённые изменения", "Modifications non enregistrées", "Ungespeicherte Änderungen");
        add("文件 %1 有未保存的更改，是否保存？", "File %1 has unsaved changes. Save?", "Файл %1 содержит несохранённые изменения. Сохранить?", "Le fichier %1 a des modifications non enregistrées. Enregistrer ?", "Datei %1 hat ungespeicherte Änderungen. Speichern?");
        add("资产 %1 有未保存的更改。", "Asset %1 has unsaved changes.", "Актив %1 содержит несохранённые изменения.", "L'actif %1 a des modifications non enregistrées.", "Asset %1 hat ungespeicherte Änderungen.");

        // ─── 资产查看器（asset_viewers.cpp）──────────────────────
        add("音频播放 — %1", "Audio Player — %1", "Аудиоплеер — %1", "Lecteur audio — %1", "Audio-Player — %1");
        add("播放 / 暂停", "Play / Pause", "Воспроизвести / Пауза", "Lecture / Pause", "Wiedergabe / Pause");
        add("播放中", "Playing", "Воспроизведение", "Lecture", "Wiedergabe");
        add("已暂停", "Paused", "Пауза", "En pause", "Pausiert");
        add("图片预览 — %1", "Image Viewer — %1", "Просмотр изображения — %1", "Visionneuse d'image — %1", "Bildanzeige — %1");
        add("无法加载图片", "Cannot load image", "Не удалось загрузить изображение", "Impossible de charger l'image", "Bild kann nicht geladen werden");
        add("模型查看器 — %1", "Model Viewer — %1", "Просмотр модели — %1", "Visionneuse de modèle — %1", "Modellansicht — %1");
        add("顶点: %1   三角形: %2", "Vertices: %1   Triangles: %2", "Вершины: %1   Треугольники: %2", "Sommets : %1   Triangles : %2", "Eckpunkte: %1   Dreiecke: %2");
        add("（无法渲染模型）", "(Cannot render model)", "(Не удалось отрисовать модель)", "(Impossible de rendre le modèle)", "(Modell kann nicht gerendert werden)");
        add("加载失败: %1", "Load failed: %1", "Ошибка загрузки: %1", "Échec du chargement : %1", "Laden fehlgeschlagen: %1");
        add("（模型加载失败）", "(Model load failed)", "(Ошибка загрузки модели)", "(Échec du chargement du modèle)", "(Modellladen fehlgeschlagen)");

        load_saved();
    }
}