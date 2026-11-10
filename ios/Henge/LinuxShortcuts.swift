import SwiftUI

/// A key chord the user has named, sent to a Linux machine from the session's shortcuts button: what a hardware
/// keyboard cannot type because iPadOS keeps the chord (⌘-Space), and what a touch screen has no keys for at all.
struct LinuxShortcut: Codable, Identifiable, Hashable {
    var id = UUID()
    var name: String
    var superKey = true
    var control = false
    var alt = false
    var shift = false
    /// A Linux key code (as henge_vm_key takes), one of LinuxShortcut.keys.
    var key: UInt16

    /// The keys a shortcut can end in, by name, with their Linux key codes.
    static let keys: [(name: String, code: UInt16)] = {
        var k: [(String, UInt16)] = [("Space", 57), ("Return", 28), ("Tab", 15), ("Escape", 1), ("Backspace", 14), ("Delete", 111)]
        let rows: [(String, [UInt16])] = [
            ("QWERTYUIOP", [16, 17, 18, 19, 20, 21, 22, 23, 24, 25]),
            ("ASDFGHJKL", [30, 31, 32, 33, 34, 35, 36, 37, 38]),
            ("ZXCVBNM", [44, 45, 46, 47, 48, 49, 50]),
        ]
        var letters: [(String, UInt16)] = []
        for (names, codes) in rows { for (c, code) in zip(names, codes) { letters.append((String(c), code)) } }
        k += letters.sorted { $0.0 < $1.0 }
        k += (1...9).map { (String($0), UInt16($0 + 1)) } + [("0", 11)]
        k += [("Up", 103), ("Down", 108), ("Left", 105), ("Right", 106), ("Home", 102), ("End", 107), ("Page Up", 104), ("Page Down", 109)]
        k += (1...10).map { ("F\($0)", UInt16(58 + $0)) } + [("F11", 87), ("F12", 88)]
        k += [("-", 12), ("=", 13), ("[", 26), ("]", 27), (";", 39), ("'", 40), ("`", 41), ("\\", 43), (",", 51), (".", 52), ("/", 53), ("Print Screen", 99)]
        return k.map { (name: $0.0, code: $0.1) }
    }()

    var keyName: String { Self.keys.first { $0.code == key }?.name ?? "Key \(key)" }

    /// "Super-Alt-Space"
    var chord: String {
        ((superKey ? ["Super"] : []) + (control ? ["Ctrl"] : []) + (alt ? ["Alt"] : []) + (shift ? ["Shift"] : []) + [keyName])
            .joined(separator: "-")
    }

    /// The name, or the chord when it has none.
    var title: String { name.trimmingCharacters(in: .whitespaces).isEmpty ? chord : name }

    /// Modifiers down, the key, modifiers up again.
    func send() {
        let held: [UInt16] = (superKey ? [Keys.command] : []) + (control ? [Keys.control] : []) + (alt ? [Keys.option] : []) + (shift ? [Keys.shift] : [])
        for m in held { henge_vm_key(m, 1) }
        henge_vm_key(key, 1)
        henge_vm_key(key, 0)
        for m in held.reversed() { henge_vm_key(m, 0) }
    }

    /// What a new install offers: Omarchy's own bindings a keyboard on an iPad cannot reach, and the two used most.
    static let standard: [LinuxShortcut] = [
        LinuxShortcut(name: "Launcher", key: 57),
        LinuxShortcut(name: "Omarchy Menu", alt: true, key: 57),
        LinuxShortcut(name: "Terminal", key: 28),
        LinuxShortcut(name: "Close Window", key: 17),
    ]
}

/// The user's shortcuts, kept in the app's settings. The first one is what a tap on the session's button sends.
final class LinuxShortcuts: ObservableObject {
    static let shared = LinuxShortcuts()
    private static let key = "henge.linux.shortcuts"

    @Published var all: [LinuxShortcut] {
        didSet {
            if let data = try? JSONEncoder().encode(all) { UserDefaults.standard.set(data, forKey: Self.key) }
        }
    }

    private init() {
        if let data = UserDefaults.standard.data(forKey: Self.key), let saved = try? JSONDecoder().decode([LinuxShortcut].self, from: data) {
            all = saved
        } else {
            all = LinuxShortcut.standard
        }
    }
}

/// The list, for Settings and for a running Linux machine's sheet: add, change, reorder, delete.
struct LinuxShortcutsSection: View {
    @ObservedObject private var shortcuts = LinuxShortcuts.shared
    @State private var editing: LinuxShortcut?

    var body: some View {
        Section {
            ForEach(shortcuts.all) { s in
                Button { editing = s } label: {
                    HStack {
                        Text(s.title).foregroundStyle(.primary)
                        Spacer()
                        Text(s.chord).font(.system(.footnote, design: .monospaced)).foregroundStyle(.secondary)
                    }
                }
            }
            .onDelete { shortcuts.all.remove(atOffsets: $0) }
            .onMove { shortcuts.all.move(fromOffsets: $0, toOffset: $1) }
            Button("Add Shortcut…") { editing = LinuxShortcut(name: "", key: 57) }
            if shortcuts.all != LinuxShortcut.standard {
                Button("Restore the Standard Shortcuts", role: .destructive) { shortcuts.all = LinuxShortcut.standard }
            }
        } header: {
            HStack {
                Text("Linux Shortcuts")
                Spacer()
                EditButton().font(.footnote).textCase(nil)
            }
        } footer: {
            Text("Key chords for the shortcuts button in a Linux machine's controls: tap the button for the first one, hold it for the rest. For chords a keyboard cannot type here (\(Device.name == "iPad" ? "iPadOS" : "iOS") keeps Command-Space) and for a touch screen.")
        }
        .sheet(item: $editing) { s in
            LinuxShortcutEditor(shortcut: s) { changed in
                if let i = shortcuts.all.firstIndex(where: { $0.id == changed.id }) { shortcuts.all[i] = changed }
                else { shortcuts.all.append(changed) }
            }
        }
    }
}

private struct LinuxShortcutEditor: View {
    @State var shortcut: LinuxShortcut
    let save: (LinuxShortcut) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("Name") {
                    TextField(shortcut.chord, text: $shortcut.name)
                }
                Section("Keys") {
                    Toggle("Super", isOn: $shortcut.superKey)
                    Toggle("Control", isOn: $shortcut.control)
                    Toggle("Alt", isOn: $shortcut.alt)
                    Toggle("Shift", isOn: $shortcut.shift)
                    Picker("Key", selection: $shortcut.key) {
                        ForEach(LinuxShortcut.keys, id: \.code) { Text($0.name).tag($0.code) }
                    }
                }
                Section {
                    LabeledContent("Sends", value: shortcut.chord)
                }
            }
            .navigationTitle("Shortcut")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        save(shortcut)
                        dismiss()
                    }
                }
            }
        }
    }
}
