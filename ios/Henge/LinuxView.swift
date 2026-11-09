import SwiftUI
import UniformTypeIdentifiers

/// Linux machines, beside the Macs: the one used last with Start, the others, and adding one
/// (Omarchy's image, or a disk image from Files).
struct LinuxView: View {
    @ObservedObject private var machines = Machines.shared
    @ObservedObject private var installer = LinuxInstaller.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @Binding var running: VirtualMac?
    @State private var jitReady = JITBootstrap.ready
    @State private var chosen: UUID?
    @State private var picking = false
    @State private var ramGB = min(Device.suggestedRAMGB, 4)
    @AppStorage("henge.cpus") private var cpus = 4

    private var linuxes: [VirtualMac] { machines.all.filter(\.isLinux) }

    private var shown: VirtualMac? {
        if let chosen, let m = linuxes.first(where: { $0.id == chosen }) { return m }
        return linuxes.max { ($0.lastUsed ?? $0.added) < ($1.lastUsed ?? $1.added) }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    progress
                    if let m = shown {
                        hero(m)
                        others(besides: m)
                    }
                    add
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
                .deskColumn()
            }
            .background(Desk.page)
            .navigationTitle("Linux")
            .navigationDestination(for: VirtualMac.self) { LinuxSettingsView(machine: $0) }
            .fileImporter(isPresented: $picking, allowedContentTypes: [.data, .diskImage]) { result in
                if case let .success(url) = result { installer.importDisk(url, ramMB: ramGB * 1024) }
            }
            .task {
                while !Task.isCancelled {
                    jitReady = JITBootstrap.ready
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }

    // MARK: The machine on show

    private func hero(_ m: VirtualMac) -> some View {
        VStack(spacing: 0) {
            NavigationLink(value: m) {
                VStack(spacing: 0) {
                    ZStack {
                        Desk.raised.opacity(0.5)
                        if let image = UIImage(contentsOfFile: MacPreview.file(for: m).path) {
                            Image(uiImage: image).resizable().aspectRatio(contentMode: .fill)
                        } else {
                            Image(systemName: "terminal")
                                .font(.system(size: 46, weight: .light))
                                .foregroundStyle(Desk.faint)
                        }
                    }
                    .aspectRatio(m.aspect, contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(Color.white.opacity(0.14), lineWidth: 0.5))
                    HStack {
                        Text(m.name).font(.system(size: 24, weight: .bold)).foregroundStyle(Desk.text).lineLimit(1)
                        Spacer()
                        Text(jitReady ? "JIT on" : "JIT off")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(jitReady ? Desk.good : Desk.warn)
                            .padding(.horizontal, 11)
                            .padding(.vertical, 5)
                            .background((jitReady ? Desk.good : Desk.warn).opacity(0.13), in: Capsule())
                        Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Desk.faint)
                    }
                    .padding(.top, 15)
                    .padding(.horizontal, 5)
                    Text("\(m.summary) · \(cpus) CPUs · \(ByteText.string(UInt64(max(m.bytesUsed, 0)))) on disk")
                        .font(.system(size: 15))
                        .foregroundStyle(Desk.dim)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 5)
                        .padding(.top, 3)
                }
            }
            .buttonStyle(.plain)
            if jitReady {
                Button { running = m } label: { Label("Start", systemImage: "play.fill") }
                    .buttonStyle(DeskButtonStyle())
                    .padding(.top, 15)
            } else {
                Text(JITBootstrap.lastFailure ?? jit.automaticDescription).font(.system(size: 13)).foregroundStyle(Desk.dim)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 5)
                    .padding(.top, 10)
                Button("Turn On JIT") { jit.enable() }
                    .buttonStyle(DeskButtonStyle())
                    .padding(.top, 15)
            }
        }
        .padding(14)
        .deskCard(radius: 30)
    }

    @ViewBuilder private func others(besides shown: VirtualMac) -> some View {
        let rest = linuxes.filter { $0.id != shown.id }
        if !rest.isEmpty {
            DeskHeading("Other Linux machines")
            VStack(spacing: 0) {
                ForEach(rest) { other in
                    Button { chosen = other.id } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(other.name).font(.system(size: 16, weight: .medium)).foregroundStyle(Desk.text)
                                Text(other.summary).font(.system(size: 13)).foregroundStyle(Desk.dim)
                            }
                            Spacer()
                            Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Desk.faint)
                        }
                        .padding(.horizontal, 17)
                        .padding(.vertical, 12)
                        .contentShape(Rectangle())
                    }
                    if other.id != rest.last?.id { Rectangle().fill(Desk.hairline).frame(height: 0.5) }
                }
            }
            .deskCard()
        }
    }

    // MARK: Adding one

    @ViewBuilder private var progress: some View {
        switch installer.phase {
        case .finding:
            NoticeRow(symbol: "magnifyingglass", text: "Finding Omarchy's latest image…")
        case let .downloading(part, of):
            VStack(alignment: .leading, spacing: 10) {
                Text("Downloading part \(part) of \(of): \(ByteText.string(UInt64(installer.received))) of \(ByteText.string(UInt64(installer.expected)))")
                    .font(.system(size: 15))
                DeskBar(fraction: Double(installer.received) / Double(max(installer.expected, 1)))
                Text("Keep Henge open. The screen stays on until it is done.").font(.system(size: 13)).foregroundStyle(Desk.dim)
            }
            .padding(15)
            .deskCard()
        case .checking:
            NoticeRow(symbol: "checkmark.shield", text: "Checking the download…")
        case .unpacking:
            VStack(alignment: .leading, spacing: 10) {
                Text(String(format: "Unpacking the disk… %.0f%%", installer.unpacked * 100)).font(.system(size: 15))
                DeskBar(fraction: installer.unpacked)
            }
            .padding(15)
            .deskCard()
        case let .done(text):
            Button { installer.reset() } label: { NoticeRow(symbol: "checkmark.circle.fill", text: text, trailing: "OK", tint: Desk.good) }
        case let .failed(text):
            Button { installer.reset() } label: { NoticeRow(symbol: "exclamationmark.triangle.fill", text: text, trailing: "OK", tint: Desk.bad) }
        case .idle:
            EmptyView()
        }
        if let name = installer.importing {
            NoticeRow(symbol: "square.and.arrow.down.fill", text: "Bringing in \(name)…")
        }
    }

    private var add: some View {
        VStack(alignment: .leading, spacing: 14) {
            if linuxes.isEmpty {
                Image(systemName: "terminal")
                    .font(.system(size: 54, weight: .light))
                    .foregroundStyle(Desk.accentLight)
                    .frame(maxWidth: .infinity)
                    .frame(height: 150)
                    .background(Desk.raised.opacity(0.5), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                Text("No Linux yet").font(.system(size: 24, weight: .bold)).frame(maxWidth: .infinity)
            } else {
                Text("Add Linux").font(.system(size: 20, weight: .bold))
            }
            ForEach(LinuxDistribution.all) { d in
                VStack(alignment: .leading, spacing: 4) {
                    Text(d.name).font(.system(size: 17, weight: .semibold))
                    Text(d.note).font(.system(size: 13)).foregroundStyle(Desk.dim)
                    Text("Needs about \(ByteText.string(UInt64(d.diskBytes + (3 << 30)))) free while it installs, \(ByteText.string(UInt64(d.diskBytes))) after.")
                        .font(.system(size: 13)).foregroundStyle(Desk.dim)
                }
                Stepper("Memory: \(ramGB) GB", value: $ramGB, in: 2...max(Device.suggestedRAMGB, 2))
                    .font(.system(size: 15))
                Button("Install \(d.name)") {
                    let n = linuxes.filter { $0.name.hasPrefix(d.name) }.count
                    installer.install(d, name: n == 0 ? d.name : "\(d.name) \(n + 1)", ramMB: ramGB * 1024)
                }
                .buttonStyle(DeskButtonStyle(enabled: !installer.busy))
                .disabled(installer.busy)
            }
            Button("Bring in a Linux disk image…") { picking = true }
                .font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight)
                .frame(maxWidth: .infinity)
                .disabled(installer.busy)
            Text("A raw disk (.img) whose EFI partition boots with Limine, as Omarchy's does. Henge starts the kernel its limine.conf names; there is no UEFI.")
                .font(.system(size: 12)).foregroundStyle(Desk.faint)
        }
        .padding(16)
        .deskCard(radius: 30)
    }
}

/// One Linux machine: its name, memory and CPUs, and deleting it.
struct LinuxSettingsView: View {
    @ObservedObject private var machines = Machines.shared
    @Environment(\.dismiss) private var dismiss
    @State private var machine: VirtualMac
    @State private var confirmDelete = false
    @AppStorage("henge.cpus") private var cpus = 4

    init(machine: VirtualMac) { _machine = State(initialValue: machine) }

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                DeskHeading("Name")
                TextField("Name", text: $machine.name)
                    .font(.system(size: 16))
                    .submitLabel(.done)
                    .padding(.horizontal, 17)
                    .padding(.vertical, 14)
                    .deskCard()
                    .onSubmit { machines.update(machine) }

                DeskHeading("Memory")
                Stepper("\(machine.ramMB / 1024) GB", value: Binding(get: { machine.ramMB / 1024 },
                                                                       set: { machine.ramMB = $0 * 1024; machines.update(machine) }),
                        in: Device.memoryChoices)
                    .font(.system(size: 16))
                    .padding(.horizontal, 17)
                    .padding(.vertical, 10)
                    .deskCard()

                DeskHeading("Processors")
                Stepper("\(cpus) CPUs", value: $cpus, in: 1...6)
                    .font(.system(size: 16))
                    .padding(.horizontal, 17)
                    .padding(.vertical, 10)
                    .deskCard()

                DeskHeading("Screen")
                Text("\(machine.screen.width) × \(machine.screen.height), drawn by Linux's own CPUs (simple-framebuffer: there is no GPU for Linux yet).")
                    .font(.system(size: 14)).foregroundStyle(Desk.dim)
                    .padding(17)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .deskCard()

                DeskHeading("")
                Button(role: .destructive) { confirmDelete = true } label: {
                    Label("Delete \(machine.name)", systemImage: "trash").frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(17)
                .deskCard()
            }
            .padding(.horizontal, 16)
            .deskColumn()
        }
        .background(Desk.page)
        .navigationTitle(machine.name)
        .confirmationDialog("Delete \(machine.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                machines.delete(machine.id)
                dismiss()
            }
        } message: {
            Text("Its disk is removed from this \(Device.name).")
        }
    }
}
