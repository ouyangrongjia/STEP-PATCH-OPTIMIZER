#include "app/MainWindow.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QStyleFactory>

namespace {

QString buildApplicationStyleSheet() {
    return QStringLiteral(R"(
        QMainWindow {
            background: #101820;
        }

        QMenuBar {
            background: #0f1720;
            color: #d7e0ea;
            border-bottom: 1px solid #273241;
            padding: 3px 8px;
            spacing: 4px;
        }

        QMenuBar::item {
            padding: 5px 10px;
            border-radius: 4px;
        }

        QMenuBar::item:selected {
            background: #1e2a38;
        }

        QMenu {
            background: #16202b;
            color: #d7e0ea;
            border: 1px solid #2c3848;
            padding: 6px;
        }

        QMenu::item {
            padding: 6px 24px 6px 14px;
            border-radius: 4px;
        }

        QMenu::item:selected {
            background: #2c7be5;
            color: #ffffff;
        }

        QToolBar#primaryToolBar {
            background: #121c26;
            border: 0;
            border-bottom: 1px solid #273241;
            spacing: 8px;
            padding: 8px 10px;
        }

        QToolBar::separator {
            background: #314052;
            width: 1px;
            margin: 5px 8px;
        }

        QToolButton {
            color: #d7e0ea;
            background: #1a2633;
            border: 1px solid #2b3a4b;
            border-radius: 6px;
            padding: 5px 10px;
            min-height: 24px;
        }

        QToolButton:hover {
            background: #223246;
            border-color: #3c516b;
        }

        QToolButton:pressed,
        QToolButton:checked {
            background: #2c7be5;
            border-color: #2c7be5;
            color: #ffffff;
        }

        QDockWidget {
            color: #d7e0ea;
            titlebar-close-icon: none;
            titlebar-normal-icon: none;
        }

        QDockWidget::title {
            background: #121c26;
            border-top: 1px solid #273241;
            border-bottom: 1px solid #273241;
            padding: 7px 10px;
            text-align: left;
        }

        QWidget {
            color: #d7e0ea;
            selection-background-color: #2c7be5;
            selection-color: #ffffff;
        }

        QGroupBox {
            background: #16202b;
            border: 1px solid #2b3a4b;
            border-radius: 8px;
            margin-top: 14px;
            padding: 12px;
            font-weight: 600;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 6px;
            color: #9fb3c8;
        }

        QLabel#panelHint {
            color: #8fa3b8;
            background: #121c26;
            border: 1px solid #2b3a4b;
            border-radius: 6px;
            padding: 8px;
        }

        QSpinBox,
        QDoubleSpinBox,
        QComboBox,
        QLineEdit {
            background: #0f1720;
            color: #d7e0ea;
            border: 1px solid #334155;
            border-radius: 5px;
            padding: 5px 7px;
            min-height: 22px;
        }

        QSpinBox:focus,
        QDoubleSpinBox:focus,
        QComboBox:focus,
        QLineEdit:focus {
            border-color: #2c7be5;
        }

        QCheckBox {
            spacing: 7px;
        }

        QTreeWidget,
        QTableWidget,
        QPlainTextEdit {
            background: #0f1720;
            alternate-background-color: #121c26;
            color: #d7e0ea;
            border: 1px solid #273241;
            border-radius: 6px;
        }

        QHeaderView::section {
            background: #182433;
            color: #9fb3c8;
            border: 0;
            border-bottom: 1px solid #2b3a4b;
            padding: 6px 8px;
            font-weight: 600;
        }

        QTreeView::item,
        QTableView::item {
            min-height: 24px;
            padding: 3px 6px;
        }

        QTreeView::item:selected,
        QTableView::item:selected {
            background: #254b78;
            color: #ffffff;
        }

        QTabWidget::pane {
            border: 1px solid #273241;
            top: -1px;
        }

        QTabBar::tab {
            background: #121c26;
            color: #9fb3c8;
            border: 1px solid #273241;
            border-bottom: none;
            padding: 7px 16px;
            margin-right: 3px;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
        }

        QTabBar::tab:selected {
            background: #182433;
            color: #ffffff;
            border-color: #334155;
        }

        QStatusBar {
            background: #0f1720;
            color: #9fb3c8;
            border-top: 1px solid #273241;
            padding: 3px 8px;
        }

        QScrollBar:vertical,
        QScrollBar:horizontal {
            background: #0f1720;
            border: none;
            margin: 0;
        }

        QScrollBar::handle:vertical,
        QScrollBar::handle:horizontal {
            background: #34465a;
            border-radius: 4px;
            min-height: 28px;
            min-width: 28px;
        }

        QScrollBar::handle:hover {
            background: #405773;
        }

        QScrollBar::add-line,
        QScrollBar::sub-line {
            width: 0;
            height: 0;
        }
    )");
}

void applyApplicationTheme(QApplication& app) {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    app.setFont(font);

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(16, 24, 32));
    palette.setColor(QPalette::WindowText, QColor(215, 224, 234));
    palette.setColor(QPalette::Base, QColor(15, 23, 32));
    palette.setColor(QPalette::AlternateBase, QColor(18, 28, 38));
    palette.setColor(QPalette::Text, QColor(215, 224, 234));
    palette.setColor(QPalette::Button, QColor(26, 38, 51));
    palette.setColor(QPalette::ButtonText, QColor(215, 224, 234));
    palette.setColor(QPalette::Highlight, QColor(44, 123, 229));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    app.setPalette(palette);

    app.setStyleSheet(buildApplicationStyleSheet());
}

}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    applyApplicationTheme(app);

    spo::MainWindow window;
    window.show();
    return QApplication::exec();
}
