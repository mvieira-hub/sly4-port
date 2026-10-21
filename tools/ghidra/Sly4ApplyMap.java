// Applies the eboot_scan.py results to the open EBOOT.elf program.
//   - sets r2 (TOC) on every executable block
//   - creates functions from functions.csv
//   - names import stubs from imports.csv
//   - names the functions listed in tag_xrefs.md (Sly4_Register_TEXR_TXRH, ...): they
//     register the chunk handlers; Sly4Export.java registry:0x0055902c names the handlers
// Run from the Script Manager and pick the work\eboot folder when asked, or
// headless as a pre-script with the folder as argument (see run_ghidra.ps1).
//@category Sly4

import java.io.File;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class Sly4ApplyMap extends GhidraScript {

	private static final long TOC = 0xF50800L;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File folder = args.length > 0 ? new File(args[0])
				: askDirectory("Pasta work\\eboot (saida do eboot_scan.py)", "Usar");
		setToc();

		int created = 0;
		for (Map<String, String> row : readCsv(new File(folder, "functions.csv"))) {
			Address a = toAddr(Long.decode(row.get("address")));
			if (getFunctionAt(a) == null) {
				disassemble(a);
				if (createFunction(a, null) != null) {
					created++;
				}
			}
		}
		println("funcoes criadas: " + created);

		int named = 0;
		for (Map<String, String> row : readCsv(new File(folder, "imports.csv"))) {
			String stub = row.get("stub");
			if (stub == null || stub.isEmpty()) {
				continue;
			}
			String name = row.get("name");
			if (name == null || name.isEmpty()) {
				name = row.get("module") + "_" + row.get("nid").substring(2);
			}
			nameFunction(toAddr(Long.decode(stub)), name, "import " + row.get("module") + " NID " + row.get("nid"));
			named++;
		}
		println("stubs de import nomeados: " + named);

		// "| TEXR | 0x0051DF3C |" rows of the summary table
		Pattern p = Pattern.compile("^\\| (\\w{4}) \\| (0x[0-9A-F]{8}(?:, 0x[0-9A-F]{8})*) \\|");
		Map<String, List<String>> loaders = new LinkedHashMap<>();
		for (String line : Files.readAllLines(new File(folder, "tag_xrefs.md").toPath(), StandardCharsets.UTF_8)) {
			Matcher m = p.matcher(line);
			if (m.find()) {
				for (String a : m.group(2).split(", ")) {
					loaders.computeIfAbsent(a, k -> new ArrayList<>()).add(m.group(1));
				}
			}
		}
		for (Map.Entry<String, List<String>> e : loaders.entrySet()) {
			nameFunction(toAddr(Long.decode(e.getKey())), "Sly4_Register_" + String.join("_", e.getValue()),
				"referencia direta as tags: " + String.join(", ", e.getValue()));
		}
		println("funcoes de registro nomeadas: " + loaders.size());
	}

	private void setToc() throws Exception {
		ProgramContext ctx = currentProgram.getProgramContext();
		Register r2 = ctx.getRegister("r2");
		for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
			if (block.isExecute()) {
				ctx.setValue(r2, block.getStart(), block.getEnd(), BigInteger.valueOf(TOC));
			}
		}
		println(String.format("r2 = 0x%X nos blocos executaveis", TOC));
	}

	private void nameFunction(Address a, String name, String comment) throws Exception {
		Function f = getFunctionAt(a);
		if (f == null) {
			disassemble(a);
			f = createFunction(a, null);
		}
		if (f == null) {
			createLabel(a, name, true);
			return;
		}
		f.setName(name, SourceType.USER_DEFINED);
		f.setComment(comment);
	}

	/** Our CSVs have no quoted fields, so a plain split is enough. */
	private List<Map<String, String>> readCsv(File file) throws Exception {
		List<String> lines = Files.readAllLines(file.toPath(), StandardCharsets.UTF_8);
		List<Map<String, String>> rows = new ArrayList<>();
		String[] header = lines.get(0).trim().split(",", -1);
		for (String line : lines.subList(1, lines.size())) {
			if (line.trim().isEmpty()) {
				continue;
			}
			String[] cells = line.trim().split(",", -1);
			Map<String, String> row = new HashMap<>();
			for (int i = 0; i < header.length && i < cells.length; i++) {
				row.put(header[i], cells[i]);
			}
			rows.add(row);
		}
		return rows;
	}
}
