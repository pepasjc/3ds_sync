from PyQt6.QtWidgets import (
    QDialog,
    QHeaderView,
    QHBoxLayout,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)
from PyQt6.QtCore import Qt

from config import load_config, save_config
from dialogs.profile_dialog import ProfileDialog
from rom_installer import ROM_FORMAT_LABELS


def _profile_path_display(profile: dict) -> str:
    if profile.get("device_type") == "MemCard Pro FTP":
        host = profile.get("ftp_host", "")
        port = profile.get("ftp_port", 21)
        path = profile.get("path", "/") or "/"
        try:
            port_num = int(port or 21)
        except (TypeError, ValueError):
            port_num = 21
        port_text = "" if port_num == 21 else f":{port_num}"
        return f"ftp://{host}{port_text}{path}"
    return profile.get("path", "")


def _profile_rom_format_display(profile: dict) -> str:
    if "systems" in profile:
        values = {
            str(s.get("rom_format", "auto") or "auto").lower()
            for s in profile.get("systems", [])
            if s.get("enabled", True)
        }
        if len(values) == 1:
            return ROM_FORMAT_LABELS.get(next(iter(values)), "Auto")
        return "Mixed" if values else "Auto"
    value = str(profile.get("rom_format", "auto") or "auto").lower()
    return ROM_FORMAT_LABELS.get(value, "Auto")


class ProfilesTab(QWidget):
    def __init__(self):
        super().__init__()
        self._init_ui()
        self._load_profiles()

    def _init_ui(self):
        layout = QVBoxLayout(self)

        btn_row = QHBoxLayout()
        add_btn = QPushButton("Add Profile")
        add_btn.clicked.connect(self._add_profile)
        edit_btn = QPushButton("Edit Profile")
        edit_btn.clicked.connect(self._edit_profile)
        del_btn = QPushButton("Delete Profile")
        del_btn.clicked.connect(self._delete_profile)
        btn_row.addWidget(add_btn)
        btn_row.addWidget(edit_btn)
        btn_row.addWidget(del_btn)
        btn_row.addStretch()
        layout.addLayout(btn_row)

        self.table = QTableWidget()
        self.table.setColumnCount(5)
        self.table.setHorizontalHeaderLabels(["Name", "Device Type", "Game Folder", "Save Folder", "ROM Format"])
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        layout.addWidget(self.table)

    def _load_profiles(self):
        config = load_config()
        profiles = config.get("profiles", [])
        self.table.setRowCount(0)
        for p in profiles:
            row = self.table.rowCount()
            self.table.insertRow(row)
            self.table.setItem(row, 0, QTableWidgetItem(p.get("name", "")))
            self.table.setItem(row, 1, QTableWidgetItem(p.get("device_type", "")))
            self.table.setItem(row, 2, QTableWidgetItem(_profile_path_display(p)))
            save_folder = p.get("save_folder", "")
            self.table.setItem(row, 3, QTableWidgetItem(save_folder or "(same as game folder)"))
            self.table.setItem(row, 4, QTableWidgetItem(_profile_rom_format_display(p)))
            self.table.item(row, 0).setData(Qt.ItemDataRole.UserRole, p)

    def _save_profiles(self):
        profiles = []
        for row in range(self.table.rowCount()):
            item = self.table.item(row, 0)
            if item:
                profiles.append(item.data(Qt.ItemDataRole.UserRole))
        config = load_config()
        config["profiles"] = profiles
        save_config(config)

    def get_profiles(self) -> list[dict]:
        profiles = []
        for row in range(self.table.rowCount()):
            item = self.table.item(row, 0)
            if item:
                profiles.append(item.data(Qt.ItemDataRole.UserRole))
        return profiles

    def _add_profile(self):
        dialog = ProfileDialog(parent=self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            profile = dialog.get_profile()
            row = self.table.rowCount()
            self.table.insertRow(row)
            self.table.setItem(row, 0, QTableWidgetItem(profile["name"]))
            self.table.setItem(row, 1, QTableWidgetItem(profile["device_type"]))
            self.table.setItem(row, 2, QTableWidgetItem(_profile_path_display(profile)))
            sf = profile.get("save_folder", "")
            self.table.setItem(row, 3, QTableWidgetItem(sf or "(same as game folder)"))
            self.table.setItem(row, 4, QTableWidgetItem(_profile_rom_format_display(profile)))
            self.table.item(row, 0).setData(Qt.ItemDataRole.UserRole, profile)
            self._save_profiles()

    def _edit_profile(self):
        row = self.table.currentRow()
        if row < 0:
            return
        profile = self.table.item(row, 0).data(Qt.ItemDataRole.UserRole)
        dialog = ProfileDialog(profile=profile, parent=self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            updated = dialog.get_profile()
            self.table.setItem(row, 0, QTableWidgetItem(updated["name"]))
            self.table.setItem(row, 1, QTableWidgetItem(updated["device_type"]))
            self.table.setItem(row, 2, QTableWidgetItem(_profile_path_display(updated)))
            sf = updated.get("save_folder", "")
            self.table.setItem(row, 3, QTableWidgetItem(sf or "(same as game folder)"))
            self.table.setItem(row, 4, QTableWidgetItem(_profile_rom_format_display(updated)))
            self.table.item(row, 0).setData(Qt.ItemDataRole.UserRole, updated)
            self._save_profiles()

    def _delete_profile(self):
        row = self.table.currentRow()
        if row < 0:
            return
        name = self.table.item(row, 0).text()
        reply = QMessageBox.question(
            self, "Confirm Delete", f"Delete profile '{name}'?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if reply == QMessageBox.StandardButton.Yes:
            self.table.removeRow(row)
            self._save_profiles()
