/*
    Copyright (c) 2013, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef SHORTCUTDIALOG_H
#define SHORTCUTDIALOG_H

#include <QDialog>

namespace Ui {
class ShortcutDialog;
}

class ShortcutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShortcutDialog(QWidget *parent = NULL);
    ~ShortcutDialog();

    QKeySequence shortcut() const;

protected:
    void keyPressEvent(QKeyEvent *event);
    void keyReleaseEvent(QKeyEvent *event);

private:
    Ui::ShortcutDialog *ui;
    QKeySequence m_shortcut;

    void processKey(int key, Qt::KeyboardModifiers mods);
};

#endif // SHORTCUTDIALOG_H
