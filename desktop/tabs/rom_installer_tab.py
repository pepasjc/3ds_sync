from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import Qt, QThread, pyqtSignal
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QComboBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from rom_installer import (
    ROM_FORMAT_OPTIONS,
    available_systems_for_profiles,
    build_install_plan,
    fetch_rom_catalog,
    install_rom,
    profile_rom_format,
)


def _fmt_size(num_bytes: int) -> str:
    if num_bytes <= 0:
        return ""
    if num_bytes < 1024 * 1024:
        return f"{num_bytes / 1024:.0f} KB"
    if num_bytes < 1024 * 1024 * 1024:
        return f"{num_bytes / (1024 * 1024):.1f} MB"
    return f"{num_bytes / (1024 * 1024 * 1024):.2f} GB"


class CatalogFetchWorker(QThread):
    finished = pyqtSignal(list)
    error = pyqtSignal(str)

    def __init__(self, system: str, search: str, parent=None):
        super().__init__(parent)
        self.system = system
        self.search = search

    def run(self):
        try:
            self.finished.emit(fetch_rom_catalog(self.system, self.search))
        except Exception as exc:
            self.error.emit(str(exc) or exc.__class__.__name__)


class InstallWorker(QThread):
    progress = pyqtSignal(int, int)
    finished = pyqtSignal(list)
    error = pyqtSignal(str)

    def __init__(self, plan, parent=None):
        super().__init__(parent)
        self.plan = plan

    def run(self):
        try:
            paths = install_rom(self.plan, progress_callback=self.progress.emit)
            self.finished.emit([str(p) for p in paths])
        except Exception as exc:
            self.error.emit(str(exc) or exc.__class__.__name__)


class RomInstallerTab(QWidget):
    def __init__(self, profiles_tab):
        super().__init__()
        self.profiles_tab = profiles_tab
        self._profiles: list[dict] = []
        self._roms: list[dict] = []
        self._fetch_worker: CatalogFetchWorker | None = None
        self._install_worker: InstallWorker | None = None
        self._init_ui()
        self.refresh_profiles()

    def _init_ui(self):
        layout = QVBoxLayout(self)

        top = QHBoxLayout()
        top.addWidget(QLabel("Profile:"))
        self.profile_combo = QComboBox()
        self.profile_combo.currentIndexChanged.connect(self._on_profile_changed)
        top.addWidget(self.profile_combo, 2)

        top.addWidget(QLabel("System:"))
        self.system_combo = QComboBox()
        self.system_combo.currentIndexChanged.connect(self._on_system_changed)
        top.addWidget(self.system_combo)

        top.addWidget(QLabel("Format:"))
        self.format_combo = QComboBox()
        for value, label in ROM_FORMAT_OPTIONS:
            self.format_combo.addItem(label, value)
        self.format_combo.currentIndexChanged.connect(
            lambda _idx: self._populate_table(self._roms) if self._roms else None
        )
        top.addWidget(self.format_combo)
        layout.addLayout(top)

        search_row = QHBoxLayout()
        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText("Search catalog by game name or filename...")
        self.search_edit.returnPressed.connect(self.load_catalog)
        search_row.addWidget(self.search_edit, 1)
        self.fetch_btn = QPushButton("Fetch Catalog")
        self.fetch_btn.clicked.connect(self.load_catalog)
        search_row.addWidget(self.fetch_btn)
        self.install_btn = QPushButton("Install Selected")
        self.install_btn.clicked.connect(self.install_selected)
        search_row.addWidget(self.install_btn)
        layout.addLayout(search_row)

        self.status_label = QLabel("")
        layout.addWidget(self.status_label)

        self.table = QTableWidget(0, 6)
        self.table.setHorizontalHeaderLabels(
            ["System", "Name", "Filename", "Size", "Install Format", "ROM ID"]
        )
        hdr = self.table.horizontalHeader()
        hdr.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        hdr.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        hdr.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        hdr.setSectionResizeMode(3, QHeaderView.ResizeMode.Fixed)
        hdr.setSectionResizeMode(4, QHeaderView.ResizeMode.Fixed)
        hdr.setSectionResizeMode(5, QHeaderView.ResizeMode.ResizeToContents)
        self.table.setColumnWidth(0, 80)
        self.table.setColumnWidth(3, 90)
        self.table.setColumnWidth(4, 120)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.doubleClicked.connect(lambda _idx: self.install_selected())
        layout.addWidget(self.table, 1)

    def refresh_profiles(self):
        current = self.profile_combo.currentText()
        self._profiles = self.profiles_tab.get_profiles()
        self.profile_combo.blockSignals(True)
        self.profile_combo.clear()
        for profile in self._profiles:
            self.profile_combo.addItem(profile.get("name", "Profile"), profile)
        if current:
            idx = self.profile_combo.findText(current)
            if idx >= 0:
                self.profile_combo.setCurrentIndex(idx)
        self.profile_combo.blockSignals(False)
        self._on_profile_changed()

    def _current_profile(self) -> dict | None:
        idx = self.profile_combo.currentIndex()
        if idx < 0:
            return None
        profile = self.profile_combo.itemData(idx)
        return profile if isinstance(profile, dict) else None

    def _on_profile_changed(self):
        profile = self._current_profile()
        systems = available_systems_for_profiles([profile]) if profile else []
        current = self.system_combo.currentText()
        self.system_combo.blockSignals(True)
        self.system_combo.clear()
        self.system_combo.addItems(systems)
        if current:
            idx = self.system_combo.findText(current)
            if idx >= 0:
                self.system_combo.setCurrentIndex(idx)
        self.system_combo.blockSignals(False)
        self._on_system_changed()

    def _on_system_changed(self):
        profile = self._current_profile()
        system = self.system_combo.currentText()
        fmt = profile_rom_format(profile or {}, system) if profile and system else "auto"
        idx = self.format_combo.findData(fmt)
        self.format_combo.setCurrentIndex(idx if idx >= 0 else 0)
        if self._roms:
            self._populate_table(self._roms)

    def load_catalog(self):
        profile = self._current_profile()
        system = self.system_combo.currentText()
        if not profile or not system:
            QMessageBox.warning(self, "ROM Installer", "Choose a profile and system first.")
            return
        if self._fetch_worker and self._fetch_worker.isRunning():
            return
        self.fetch_btn.setEnabled(False)
        self.status_label.setText("Fetching ROM catalog...")
        self._fetch_worker = CatalogFetchWorker(system, self.search_edit.text().strip(), self)
        self._fetch_worker.finished.connect(self._on_catalog_loaded)
        self._fetch_worker.error.connect(self._on_catalog_error)
        self._fetch_worker.start()

    def _on_catalog_loaded(self, roms: list[dict]):
        self.fetch_btn.setEnabled(True)
        self._roms = roms
        self._populate_table(roms)

    def _on_catalog_error(self, message: str):
        self.fetch_btn.setEnabled(True)
        self.status_label.setText("Catalog fetch failed.")
        QMessageBox.critical(self, "ROM Installer", message)

    def _populate_table(self, roms: list[dict]):
        profile = self._current_profile()
        system = self.system_combo.currentText()
        override = str(self.format_combo.currentData() or "auto")
        self.table.setRowCount(0)
        for rom in roms:
            row = self.table.rowCount()
            self.table.insertRow(row)
            try:
                plan = build_install_plan(profile or {}, rom, system, override)
                fmt = plan.format_label
            except Exception:
                plan = None
                fmt = ""

            values = [
                rom.get("system", ""),
                rom.get("name") or rom.get("filename", ""),
                rom.get("filename", ""),
                _fmt_size(int(rom.get("size") or 0)),
                fmt,
                rom.get("rom_id") or rom.get("title_id", ""),
            ]
            for col, value in enumerate(values):
                item = QTableWidgetItem(str(value))
                if col == 0:
                    item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                if col == 3:
                    item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                self.table.setItem(row, col, item)
            self.table.item(row, 0).setData(Qt.ItemDataRole.UserRole, rom)
        self.status_label.setText(f"{len(roms)} ROM(s)")

    def install_selected(self):
        profile = self._current_profile()
        row = self.table.currentRow()
        if not profile or row < 0:
            return
        rom_item = self.table.item(row, 0)
        rom = rom_item.data(Qt.ItemDataRole.UserRole) if rom_item else None
        if not isinstance(rom, dict):
            return
        override = str(self.format_combo.currentData() or "auto")
        try:
            plan = build_install_plan(profile, rom, self.system_combo.currentText(), override)
        except Exception as exc:
            QMessageBox.critical(self, "ROM Installer", str(exc))
            return

        target_text = str(plan.target_path)
        reply = QMessageBox.question(
            self,
            "Install ROM",
            f"Install {plan.display_name} as {plan.format_label} to:\n{target_text}",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if reply != QMessageBox.StandardButton.Yes:
            return

        self.install_btn.setEnabled(False)
        self.status_label.setText("Installing ROM...")
        self._install_worker = InstallWorker(plan, self)
        self._install_worker.progress.connect(self._on_install_progress)
        self._install_worker.finished.connect(self._on_install_finished)
        self._install_worker.error.connect(self._on_install_error)
        self._install_worker.start()

    def _on_install_progress(self, downloaded: int, total: int):
        if total > 0:
            pct = int(downloaded * 100 / total)
            self.status_label.setText(f"Installing ROM... {pct}%")
        else:
            self.status_label.setText(f"Installing ROM... {_fmt_size(downloaded)}")

    def _on_install_finished(self, paths: list[str]):
        self.install_btn.setEnabled(True)
        self.status_label.setText(f"Installed {len(paths)} file(s).")
        QMessageBox.information(
            self,
            "ROM Installer",
            "Installed:\n" + "\n".join(paths[:20]),
        )

    def _on_install_error(self, message: str):
        self.install_btn.setEnabled(True)
        self.status_label.setText("Install failed.")
        QMessageBox.critical(self, "ROM Installer", message)
