// Frame Key: the PC side's small window. Keying itself is set in the
// headset (SteamVR dashboard > Frame Key); this shows which headsets answer
// on the network, which one the PC layer follows, switches the PC layer on or
// off, and shows a test box in the key colour. Runs only while it's open.
// Build: csc /target:winexe /r:System.Windows.Forms.dll /r:System.Drawing.dll FrameKey.cs
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("Frame Key")]

namespace FrameKey {

// A headset's answer to the "FPPQ1" broadcast (see the headset panel).
class HeadsetInfo {
	public string Id, Name, Address;
	public bool On, Streaming, ToYou;
	public float[] Key = { 1, 0, 1 };
	public float Tol, Soft;
	public DateTime Seen;

	public Color KeyColor {
		get { return Color.FromArgb(C(Key[0]), C(Key[1]), C(Key[2])); }
	}
	static int C(float v) { return Math.Max(0, Math.Min(255, (int)Math.Round(v * 255))); }
	public string Hex {
		get { Color c = KeyColor; return c.R.ToString("X2") + c.G.ToString("X2") + c.B.ToString("X2"); }
	}

	public static HeadsetInfo Parse(string msg, string from) {
		if (!msg.StartsWith("FPPA1\n")) return null;
		var h = new HeadsetInfo { Address = from, Seen = DateTime.Now };
		var inv = CultureInfo.InvariantCulture;
		foreach (string line in msg.Split('\n')) {
			int eq = line.IndexOf('=');
			if (eq < 0) continue;
			string k = line.Substring(0, eq), v = line.Substring(eq + 1).Trim();
			float f;
			switch (k) {
			case "id": h.Id = v; break;
			case "name": h.Name = v; break;
			case "on": h.On = v == "1"; break;
			case "streaming": h.Streaming = v == "1"; break;
			case "toyou": h.ToYou = v == "1"; break;
			case "tolerance": if (float.TryParse(v, NumberStyles.Float, inv, out f)) h.Tol = f; break;
			case "softness": if (float.TryParse(v, NumberStyles.Float, inv, out f)) h.Soft = f; break;
			case "key":
				string[] p = v.Split(' ');
				for (int i = 0; i < 3 && i < p.Length; i++)
					if (float.TryParse(p[i], NumberStyles.Float, inv, out f)) h.Key[i] = f;
				break;
			}
		}
		return string.IsNullOrEmpty(h.Id) ? null : h;
	}
}

// Asks every network for headsets. The PC asks first, so Windows' firewall
// lets the answers in without any rules.
class Discovery : IDisposable {
	readonly UdpClient udp = new UdpClient(new IPEndPoint(IPAddress.Any, 0));
	public readonly Dictionary<string, HeadsetInfo> Heads = new Dictionary<string, HeadsetInfo>();

	public Discovery() {
		udp.EnableBroadcast = true;
		udp.Client.Blocking = false;
	}
	static IEnumerable<IPAddress> Broadcasts() {
		yield return IPAddress.Broadcast;
		foreach (var ni in NetworkInterface.GetAllNetworkInterfaces()) {
			if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
			foreach (var u in ni.GetIPProperties().UnicastAddresses) {
				if (u.Address.AddressFamily != AddressFamily.InterNetwork || u.PrefixLength <= 0 || u.PrefixLength >= 31) continue;
				byte[] a = u.Address.GetAddressBytes();
				uint ip = (uint)(a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3]);
				uint b = ip | ~(0xffffffffu << (32 - u.PrefixLength));
				yield return new IPAddress(new[] { (byte)(b >> 24), (byte)(b >> 16), (byte)(b >> 8), (byte)b });
			}
		}
	}
	public void Ask() {
		byte[] q = Encoding.ASCII.GetBytes("FPPQ1");
		foreach (IPAddress b in Broadcasts())
			try { udp.Send(q, q.Length, new IPEndPoint(b, 47911)); } catch (SocketException) { }
	}
	// Collect answers; one per network, merged per headset.
	public void Collect() {
		while (true) {
			IPEndPoint from = null;
			byte[] d;
			try {
				if (udp.Available == 0) break;
				d = udp.Receive(ref from);
			} catch (SocketException) { break; }
			HeadsetInfo h = HeadsetInfo.Parse(Encoding.UTF8.GetString(d), from.Address.ToString());
			if (h == null) continue;
			HeadsetInfo old;
			if (Heads.TryGetValue(h.Id, out old) && (DateTime.Now - old.Seen).TotalSeconds < 1.5) {
				if (old.ToYou && !h.ToYou) { old.Seen = h.Seen; continue; } // keep the address it streams on
			}
			Heads[h.Id] = h;
		}
		foreach (string id in Heads.Keys.Where(k => (DateTime.Now - Heads[k].Seen).TotalSeconds > 6).ToList())
			Heads.Remove(id);
	}
	public void Dispose() { udp.Close(); }
}

// alphakey.ini next to this exe: enabled and which headset to follow.
static class PcLayer {
	public static string Dir { get { return Path.GetDirectoryName(Application.ExecutablePath); } }
	static string Ini { get { return Path.Combine(Dir, "alphakey.ini"); } }

	public static bool Installed() {
		var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Khronos\OpenXR\1\ApiLayers\Implicit");
		if (k == null) return false;
		using (k) return k.GetValue(Path.Combine(Dir, "alphakey.json")) != null;
	}
	static List<string> Lines() { return File.Exists(Ini) ? File.ReadAllLines(Ini).ToList() : new List<string>(); }
	public static string Get(string key, string def) {
		foreach (string l in Lines()) {
			string t = l.Trim();
			int eq = t.IndexOf('=');
			if (!t.StartsWith("#") && eq > 0 && t.Substring(0, eq).Trim() == key) return t.Substring(eq + 1).Trim();
		}
		return def;
	}
	// Change one setting, keeping the rest of the file (and its comments).
	public static void Set(string key, string value) {
		List<string> lines = Lines();
		bool found = false;
		for (int i = 0; i < lines.Count; i++) {
			string t = lines[i].Trim();
			int eq = t.IndexOf('=');
			if (!t.StartsWith("#") && eq > 0 && t.Substring(0, eq).Trim() == key) { lines[i] = key + " = " + value; found = true; }
		}
		if (!found) lines.Add(key + " = " + value);
		string tmp = Ini + ".tmp";
		File.WriteAllLines(tmp, lines);
		if (File.Exists(Ini)) File.Replace(tmp, Ini, null); else File.Move(tmp, Ini);
	}
}

class MainForm : Form {
	readonly Discovery disco = new Discovery();
	readonly ListView list = new ListView();
	readonly ComboBox follow = new ComboBox();
	readonly Panel swatch = new Panel();
	readonly Label keyInfo = new Label(), layerInfo = new Label(), status = new Label();
	readonly CheckBox layerOn = new CheckBox(), testBox = new CheckBox();
	readonly Timer timer = new Timer();
	Process box;
	string boxColour;
	bool loading;

	public MainForm() {
		Text = "Frame Key";
		Font = new Font("Segoe UI", 10f);
		FormBorderStyle = FormBorderStyle.FixedSingle;
		MaximizeBox = false;
		ClientSize = new Size(600, 470);
		try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

		var hint = new Label();
		hint.Text = "Keying is set in the headset: SteamVR dashboard > Frame Key.";
		hint.SetBounds(16, 12, 570, 22);
		Controls.Add(hint);

		Header("Headsets on the network", 44);
		list.SetBounds(16, 72, 568, 130);
		list.View = View.Details;
		list.FullRowSelect = true;
		list.HeaderStyle = ColumnHeaderStyle.Nonclickable;
		list.Columns.Add("Headset", 140);
		list.Columns.Add("Keying", 70);
		list.Columns.Add("Key colour", 100);
		list.Columns.Add("Stream", 150);
		list.Columns.Add("Address", 100);
		Controls.Add(list);

		Header("This PC", 216);
		var fl = new Label();
		fl.Text = "Follow";
		fl.SetBounds(16, 248, 60, 22);
		follow.SetBounds(80, 245, 340, 28);
		follow.DropDownStyle = ComboBoxStyle.DropDownList;
		follow.SelectedIndexChanged += (o, e) => { if (!loading) SaveFollow(); };
		Controls.AddRange(new Control[] { fl, follow });
		swatch.SetBounds(16, 284, 64, 48);
		swatch.BorderStyle = BorderStyle.FixedSingle;
		keyInfo.SetBounds(90, 284, 494, 48);
		Controls.AddRange(new Control[] { swatch, keyInfo });

		layerOn.SetBounds(16, 344, 560, 26);
		layerOn.Text = "Turn OpenXR apps' transparency into the key colour";
		layerOn.CheckedChanged += (o, e) => { if (!loading) Save("enabled", layerOn.Checked ? "1" : "0"); };
		layerInfo.SetBounds(36, 370, 548, 22);
		layerInfo.ForeColor = Color.DimGray;
		Controls.AddRange(new Control[] { layerOn, layerInfo });

		testBox.SetBounds(16, 400, 400, 30);
		testBox.Appearance = Appearance.Button;
		testBox.Text = "Show a test box in the headset";
		testBox.CheckedChanged += (o, e) => UpdateTestBox();
		Controls.Add(testBox);

		status.SetBounds(16, 440, 570, 22);
		status.ForeColor = Color.DimGray;
		Controls.Add(status);

		loading = true;
		layerOn.Checked = PcLayer.Get("enabled", "1") != "0";
		loading = false;
		layerInfo.Text = PcLayer.Installed() ? "Applies to running games within a second." :
			"The PC layer isn't installed (run Install.cmd).";

		timer.Interval = 1000;
		int tick = 0;
		timer.Tick += (o, e) => {
			if (tick++ % 2 == 0) disco.Ask();
			disco.Collect();
			RefreshHeadsets();
		};
		timer.Start();
		disco.Ask();
		FormClosing += (o, e) => { StopBox(); disco.Dispose(); };
	}

	void Header(string text, int y) {
		var l = new Label();
		l.Text = text;
		l.Font = new Font(Font, FontStyle.Bold);
		l.SetBounds(16, y, 570, 22);
		Controls.Add(l);
	}

	void Save(string key, string value) {
		try { PcLayer.Set(key, value); status.Text = ""; } catch (Exception e) { status.Text = "Couldn't save: " + e.Message; }
	}
	void SaveFollow() {
		var item = follow.SelectedItem as FollowItem;
		Save("headset", item == null ? "auto" : item.Id);
	}

	class FollowItem {
		public string Id, Text;
		public override string ToString() { return Text; }
	}

	// The headset the PC layer follows, by the same rule as the layer.
	HeadsetInfo Followed() {
		var heads = disco.Heads.Values.ToList();
		var toYou = heads.FirstOrDefault(h => h.ToYou);
		if (toYou != null) return toYou;
		string pin = PcLayer.Get("headset", "auto");
		HeadsetInfo pinned;
		if (pin != "auto" && disco.Heads.TryGetValue(pin, out pinned)) return pinned;
		var streaming = heads.Where(h => h.Streaming).ToList();
		if (streaming.Count == 1) return streaming[0];
		return heads.Count == 1 ? heads[0] : null;
	}

	void RefreshHeadsets() {
		var heads = disco.Heads.Values.OrderBy(h => h.Name).ToList();
		list.BeginUpdate();
		list.Items.Clear();
		foreach (var h in heads) {
			var it = new ListViewItem(h.Name);
			it.UseItemStyleForSubItems = false;
			it.SubItems.Add(h.On ? "on" : "off");
			var c = it.SubItems.Add("#" + h.Hex);
			c.BackColor = h.KeyColor;
			c.ForeColor = h.KeyColor.GetBrightness() > 0.5 ? Color.Black : Color.White;
			it.SubItems.Add(h.ToYou ? "from this PC" : h.Streaming ? "from another PC" : "none");
			it.SubItems.Add(h.Address);
			list.Items.Add(it);
		}
		list.EndUpdate();

		// "Follow" choices: automatic plus every headset seen (the pinned one stays listed).
		string pin = PcLayer.Get("headset", "auto");
		var items = new List<FollowItem> { new FollowItem { Id = "auto", Text = "Automatic (the headset streaming from this PC)" } };
		foreach (var h in heads) items.Add(new FollowItem { Id = h.Id, Text = h.Name + "  (" + h.Id + ")" });
		if (pin != "auto" && !disco.Heads.ContainsKey(pin)) items.Add(new FollowItem { Id = pin, Text = pin + "  (not answering)" });
		string want = string.Join("|", items.Select(i => i.Id + i.Text));
		if ((string)follow.Tag != want) {
			loading = true;
			follow.Items.Clear();
			foreach (var i in items) follow.Items.Add(i);
			follow.SelectedIndex = Math.Max(0, items.FindIndex(i => i.Id == pin));
			follow.Tag = want;
			loading = false;
		}

		HeadsetInfo f = Followed();
		if (f == null) {
			swatch.BackColor = SystemColors.Control;
			keyInfo.Text = heads.Count == 0
				? "No headset answering. Is SteamVR running on the Frame, on the same network?"
				: "Several headsets and none streaming from this PC: pick one under Follow.";
		} else {
			swatch.BackColor = f.KeyColor;
			keyInfo.Text = "Following " + f.Name + ": key #" + f.Hex + ", tolerance " + f.Tol.ToString("0.00") +
				", softness " + f.Soft.ToString("0.00") + (f.On ? "" : "\r\nKeying is off in that headset.");
		}
		testBox.Enabled = f != null || testBox.Checked;
		UpdateTestBox();
	}

	// The test box follows the key colour while it's shown.
	void UpdateTestBox() {
		HeadsetInfo f = Followed();
		string colour = f != null ? f.Hex.ToLower() : boxColour;
		bool running = box != null && !box.HasExited;
		if (!testBox.Checked) {
			if (running) StopBox();
			testBox.Text = "Show a test box in the headset";
			return;
		}
		testBox.Text = "Hide the test box";
		if (running && colour == boxColour) return;
		StopBox();
		string exe = Path.Combine(PcLayer.Dir, "keybox.exe");
		if (colour == null || !File.Exists(exe)) return;
		var psi = new ProcessStartInfo(exe, "front " + colour + " 0.4 86400");
		psi.UseShellExecute = false;
		psi.CreateNoWindow = true;
		try {
			box = Process.Start(psi);
			boxColour = colour;
			status.Text = "Test box: 0.6 m ahead of where you were looking when it appeared.";
		} catch (Exception e) {
			status.Text = "Couldn't start the test box: " + e.Message;
			testBox.Checked = false;
		}
	}
	void StopBox() {
		if (box != null) {
			try { if (!box.HasExited) box.Kill(); } catch { }
			box.Dispose();
			box = null;
		}
	}

	[STAThread]
	static void Main() {
		Application.EnableVisualStyles();
		Application.SetCompatibleTextRenderingDefault(false);
		Application.Run(new MainForm());
	}
}
}
