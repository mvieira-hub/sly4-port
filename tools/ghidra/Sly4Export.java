// Exports decompiled C and a call/reference summary for selected functions.
// Arguments: <out dir> <token>...
//   prefix:NAME    every function whose name starts with NAME (e.g. prefix:Sly4_Load_)
//   0xADDR         the function containing ADDR
//   callers:0xADDR the function containing ADDR and all its direct callers
//   refs:0xADDR    every function that references ADDR (data/string address)
//   registry:0xADDR  ADDR is the chunk-handler registration function
//                  (RegisterChunkHandler(tag, &opd, ...)); writes registry.md with
//                  tag -> handler for every call and renames the handlers
//                  Sly4_Chunk_<TAG> and the callers Sly4_Register_<TAGS>
//   name:0xADDR=Name  renames the function at ADDR (kept in the project)
// Writes <out>/<name>.c per function and <out>/index.md.
//@category Sly4

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class Sly4Export extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("uso: Sly4Export.java <pasta> <tokens...>");
			return;
		}
		File out = new File(args[0]);
		out.mkdirs();

		// function -> why it was selected
		Map<Function, String> selected = new LinkedHashMap<>();
		List<String> notes = new ArrayList<>();
		for (int i = 1; i < args.length; i++) {
			if (args[i].startsWith("registry:")) {
				registry(toAddr(Long.decode(args[i].substring(9))), out);
			}
			else if (args[i].startsWith("func:")) {
				// code the analysis missed (static helpers reached only through bl): create it
				Address a = toAddr(Long.decode(args[i].substring(5)));
				if (getFunctionContaining(a) == null) {
					disassemble(a);
					createFunction(a, null);
				}
			}
			else if (args[i].startsWith("name:")) {
				String[] kv = args[i].substring(5).split("=", 2);
				Function f = getFunctionAt(toAddr(Long.decode(kv[0])));
				if (f != null) {
					f.setName(kv[1], SourceType.USER_DEFINED);
				}
			}
		}
		for (int i = 1; i < args.length; i++) {
			String t = args[i];
			if (t.startsWith("registry:") || t.startsWith("name:") || t.startsWith("func:")) {
				continue;
			}
			if (t.startsWith("prefix:")) {
				String p = t.substring(7);
				for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
					if (f.getName().startsWith(p)) {
						selected.putIfAbsent(f, t);
					}
				}
			}
			else if (t.startsWith("callers:")) {
				Function f = getFunctionContaining(toAddr(Long.decode(t.substring(8))));
				if (f != null) {
					selected.putIfAbsent(f, t);
					for (Function c : f.getCallingFunctions(monitor)) {
						selected.putIfAbsent(c, "chama " + f.getName());
					}
				}
			}
			else if (t.startsWith("refs:")) {
				Address a = toAddr(Long.decode(t.substring(5)));
				Reference[] refs = getReferencesTo(a);
				notes.add(String.format("- %s: %d referencias", a, refs.length));
				for (Reference r : refs) {
					Function f = getFunctionContaining(r.getFromAddress());
					notes.add(String.format("  - de %s (%s) em %s", r.getFromAddress(), r.getReferenceType(),
						f == null ? "sem funcao" : f.getName()));
					if (f != null) {
						selected.putIfAbsent(f, "referencia " + a);
					}
				}
			}
			else {
				Function f = getFunctionContaining(toAddr(Long.decode(t)));
				if (f != null) {
					selected.putIfAbsent(f, t);
				}
				else {
					notes.add("- nenhuma funcao em " + t);
				}
			}
		}

		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		try (PrintWriter idx = new PrintWriter(new File(out, "index.md"), StandardCharsets.UTF_8)) {
			idx.println("# Exportacao Ghidra\n");
			for (String n : notes) {
				idx.println(n);
			}
			idx.println("\n| Funcao | Endereco | Motivo | Chamada por | Chama |\n|---|---|---|---|---|");
			for (Map.Entry<Function, String> e : selected.entrySet()) {
				Function f = e.getKey();
				DecompileResults r = ifc.decompileFunction(f, 180, monitor);
				String code = r.decompileCompleted() ? r.getDecompiledFunction().getC()
						: "/* falha ao decompilar: " + r.getErrorMessage() + " */";
				String file = f.getName().replaceAll("[^A-Za-z0-9_.-]", "_") + "_" + f.getEntryPoint() + ".c";
				try (PrintWriter w = new PrintWriter(new File(out, file), StandardCharsets.UTF_8)) {
					w.println("// " + f.getName() + " @ " + f.getEntryPoint() + " (" + e.getValue() + ")");
					w.print(code);
				}
				idx.printf("| [%s](%s) | %s | %s | %s | %s |%n", f.getName(), file, f.getEntryPoint(), e.getValue(),
					names(f.getCallingFunctions(monitor)), names(f.getCalledFunctions(monitor)));
			}
		}
		ifc.dispose();
		println("exportadas " + selected.size() + " funcoes em " + out);
	}

	/** Little-endian u32 tag -> 4 ASCII chars ("0x52584554" -> "TEXR"). */
	private static String tagName(long v) {
		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < 4; i++) {
			char c = (char) ((v >> (8 * i)) & 0xFF);
			sb.append(Character.isLetterOrDigit(c) || c == '_' ? c : '?');
		}
		return sb.toString();
	}

	private void registry(Address regAddr, File out) throws Exception {
		Function reg = getFunctionContaining(regAddr);
		if (reg == null) {
			printerr("registry: nenhuma funcao em " + regAddr);
			return;
		}
		reg.setName("Sly4_RegisterChunkHandler", SourceType.USER_DEFINED);
		// 2nd argument is "&PTR_<label>_<addr>": <addr> is the handler's .opd
		// descriptor {u32 code, u32 toc}; resolve through memory so renames don't matter.
		Pattern call = Pattern.compile(Pattern.quote(reg.getName()) +
			"\\((0x[0-9a-f]+),&PTR_\\w*?_([0-9a-f]{8}),([^;]*)\\);");
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		Map<String, String> handlerTags = new TreeMap<>();
		try (PrintWriter w = new PrintWriter(new File(out, "registry.md"), StandardCharsets.UTF_8)) {
			w.println("# Tratadores de blocos registrados\n");
			w.println("| Tag | Valor | Tratador | Registrado em | Demais argumentos |\n|---|---|---|---|---|");
			for (Function caller : reg.getCallingFunctions(monitor)) {
				DecompileResults r = ifc.decompileFunction(caller, 300, monitor);
				if (!r.decompileCompleted()) {
					continue;
				}
				Matcher m = call.matcher(r.getDecompiledFunction().getC());
				List<String> tags = new ArrayList<>();
				while (m.find()) {
					String tag = tagName(Long.decode(m.group(1)));
					long code = getInt(toAddr(Long.parseLong(m.group(2), 16))) & 0xFFFFFFFFL;
					String handler = String.format("0x%08X", code);
					tags.add(tag);
					handlerTags.merge(handler, tag, (a, b) -> a + "_" + b);
					w.printf("| %s | %s | %s | %s | %s |%n", tag, m.group(1), handler, caller.getEntryPoint(),
						m.group(3).trim());
				}
				if (!tags.isEmpty() && (caller.getName().contains("FUN_") || caller.getName().startsWith("Sly4_Load_"))) {
					caller.setName("Sly4_Register_" + String.join("_", new TreeSet<>(tags)), SourceType.USER_DEFINED);
				}
			}
		}
		for (Map.Entry<String, String> e : handlerTags.entrySet()) {
			Function h = getFunctionAt(toAddr(Long.decode(e.getKey())));
			// default names may be "FUN_x" or ".opd.FUN_x"
			if (h != null && h.getName().contains("FUN_")) {
				h.setName("Sly4_Chunk_" + e.getValue(), SourceType.USER_DEFINED);
			}
		}
		ifc.dispose();
		println("registry: " + handlerTags.size() + " tratadores");
	}

	private String names(Set<Function> fs) {
		Set<String> s = new TreeSet<>();
		for (Function f : fs) {
			s.add(f.getName());
		}
		return s.size() > 12 ? s.size() + " funcoes" : String.join(", ", s);
	}
}
