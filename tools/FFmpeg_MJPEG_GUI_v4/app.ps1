# launcher.ps1 会传入 $ErrorActionPreference = "Stop"，它会把 ffmpeg 的 stderr 输出（媒体信息等）当成终止异常抛出。
# 本脚本需要以 Continue 方式捕获 ffmpeg 输出，故在此覆盖。
$ErrorActionPreference = "Continue"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

[System.Windows.Forms.Application]::EnableVisualStyles()

function Find-Tool([string]$name) {

    # ① 从当前 PowerShell 的 PATH 查找
    try {
        $cmd = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) {
            return $cmd.Source
        }
    } catch {}

    # ② 使用 where.exe 再查一次
    try {
        $result = & where.exe $name 2>$null | Select-Object -First 1
        if ($result -and (Test-Path -LiteralPath $result)) {
            return $result
        }
    } catch {}

    # ③ 检查常见 FFmpeg 安装位置
    $candidates = @(
        "$env:ProgramFiles\ffmpeg\bin\$name.exe",
        "$env:ProgramFiles\FFmpeg\bin\$name.exe",
        "${env:ProgramFiles(x86)}\ffmpeg\bin\$name.exe",
        "${env:ProgramFiles(x86)}\FFmpeg\bin\$name.exe",
        "C:\ffmpeg\bin\$name.exe",
        "C:\FFmpeg\bin\$name.exe",
        "$env:USERPROFILE\ffmpeg\bin\$name.exe",
        "$env:LOCALAPPDATA\ffmpeg\bin\$name.exe"
    )

    foreach ($path in $candidates) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    return $null
}

function Quote-Arg([string]$s) {
    return '"' + ($s -replace '"','\"') + '"'
}

function Get-DurationSeconds([string]$file, [string]$ffmpeg) {
    try {
        # 用 ffmpeg -i 解析媒体信息中的 Duration 行（该信息写到 stderr），无需 ffprobe。
        $v = & $ffmpeg -hide_banner -i $file 2>&1
        foreach ($line in $v) {
            $m = [regex]::Match($line.ToString(), 'Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)')
            if ($m.Success) {
                $ci = [System.Globalization.CultureInfo]::InvariantCulture
                $h = [double]::Parse($m.Groups[1].Value, $ci)
                $mn = [double]::Parse($m.Groups[2].Value, $ci)
                $s = [double]::Parse($m.Groups[3].Value, $ci)
                return $h * 3600 + $mn * 60 + $s
            }
        }
    } catch {}
    return 0
}

function Detect-Crop([string]$file, [string]$ffmpeg, [System.Windows.Forms.Label]$statusLabel) {
    $duration = Get-DurationSeconds $file $ffmpeg
    if ($duration -le 0) {
        $starts = @(0)
    } elseif ($duration -lt 40) {
        $starts = @(0, [Math]::Max(0, $duration * 0.45))
    } else {
        $starts = @(
            [Math]::Max(0, $duration * 0.10),
            [Math]::Max(0, $duration * 0.50),
            [Math]::Max(0, $duration * 0.80)
        )
    }

    $all = New-Object System.Collections.Generic.List[string]
    $idx = 0
    foreach ($s in $starts) {
        $idx++
        $statusLabel.Text = "正在检测黑边：样本 $idx / $($starts.Count)…"
        [System.Windows.Forms.Application]::DoEvents()

        $startText = $s.ToString("0.###", [System.Globalization.CultureInfo]::InvariantCulture)
        $lines = & $ffmpeg -hide_banner -ss $startText -i $file -t 6 -vf "cropdetect=24:16:0" -an -f null NUL 2>&1
        foreach ($line in $lines) {
            $m = [regex]::Match($line.ToString(), 'crop=\d+:\d+:\d+:\d+')
            if ($m.Success) { [void]$all.Add($m.Value) }
        }
    }

    if ($all.Count -eq 0) { return $null }

    # 取出现次数最多的裁剪结果；比简单取最后一行更不容易被黑场/字幕误导。
    $best = $all |
        Group-Object |
        Sort-Object Count -Descending |
        Select-Object -First 1

    return $best.Name
}

$ffmpeg = Find-Tool "ffmpeg"

# 当前输入视频时长（秒），选择文件时探测，用于片尾裁剪计算 -t。
$script:g_input_duration = 0

$form = New-Object System.Windows.Forms.Form
$form.Text = "FFmpeg MJPEG 视频转换器"
$form.StartPosition = "CenterScreen"
$form.Size = New-Object System.Drawing.Size(780, 740)
$form.MinimumSize = New-Object System.Drawing.Size(780, 740)
# 布局为固定坐标，禁止拖动缩放窗口，避免控件错位。
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedDialog
$form.MaximizeBox = $false
$form.MaximumSize = New-Object System.Drawing.Size(780, 740)
$form.Font = New-Object System.Drawing.Font("Microsoft YaHei UI", 9)

$y = 20

function Add-Label([string]$text, [int]$x, [int]$y, [int]$w = 110) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.Location = New-Object System.Drawing.Point($x,$y)
    $l.Size = New-Object System.Drawing.Size($w,25)
    $form.Controls.Add($l)
    return $l
}

Add-Label "输入视频：" 20 $y 90 | Out-Null
$txtInput = New-Object System.Windows.Forms.TextBox
$txtInput.Location = New-Object System.Drawing.Point(110,$y)
$txtInput.Size = New-Object System.Drawing.Size(530,25)
$form.Controls.Add($txtInput)

$btnInput = New-Object System.Windows.Forms.Button
$btnInput.Text = "选择…"
$btnInput.Location = New-Object System.Drawing.Point(650, ($y - 2))
$btnInput.Size = New-Object System.Drawing.Size(90,30)
$form.Controls.Add($btnInput)
$y += 45

Add-Label "输出文件：" 20 $y 90 | Out-Null
$txtOutput = New-Object System.Windows.Forms.TextBox
$txtOutput.Location = New-Object System.Drawing.Point(110,$y)
$txtOutput.Size = New-Object System.Drawing.Size(530,25)
$form.Controls.Add($txtOutput)

$btnOutput = New-Object System.Windows.Forms.Button
$btnOutput.Text = "另存为…"
$btnOutput.Location = New-Object System.Drawing.Point(650, ($y - 2))
$btnOutput.Size = New-Object System.Drawing.Size(90,30)
$form.Controls.Add($btnOutput)
$y += 55

$grpVideo = New-Object System.Windows.Forms.GroupBox
$grpVideo.Text = "视频参数"
$grpVideo.Location = New-Object System.Drawing.Point(20,$y)
$grpVideo.Size = New-Object System.Drawing.Size(720,155)
$form.Controls.Add($grpVideo)

$l1 = New-Object System.Windows.Forms.Label
$l1.Text = "长边长度"
$l1.Location = New-Object System.Drawing.Point(20,35)
$l1.Size = New-Object System.Drawing.Size(80,25)
$grpVideo.Controls.Add($l1)

$numWidth = New-Object System.Windows.Forms.NumericUpDown
$numWidth.Location = New-Object System.Drawing.Point(105,33)
$numWidth.Size = New-Object System.Drawing.Size(90,25)
$numWidth.Minimum = 64
$numWidth.Maximum = 4096
$numWidth.Value = 460
$grpVideo.Controls.Add($numWidth)

$l2 = New-Object System.Windows.Forms.Label
$l2.Text = "帧率 FPS"
$l2.Location = New-Object System.Drawing.Point(230,35)
$l2.Size = New-Object System.Drawing.Size(80,25)
$grpVideo.Controls.Add($l2)

$numFps = New-Object System.Windows.Forms.NumericUpDown
$numFps.Location = New-Object System.Drawing.Point(310,33)
$numFps.Size = New-Object System.Drawing.Size(80,25)
$numFps.Minimum = 1
$numFps.Maximum = 120
$numFps.Value = 24
$grpVideo.Controls.Add($numFps)

$l3 = New-Object System.Windows.Forms.Label
$l3.Text = "MJPEG q"
$l3.Location = New-Object System.Drawing.Point(415,35)
$l3.Size = New-Object System.Drawing.Size(75,25)
$grpVideo.Controls.Add($l3)

$numQ = New-Object System.Windows.Forms.NumericUpDown
$numQ.Location = New-Object System.Drawing.Point(500,33)
$numQ.Size = New-Object System.Drawing.Size(80,25)
$numQ.Minimum = 2
$numQ.Maximum = 31
$numQ.Value = 5
$grpVideo.Controls.Add($numQ)

$lblQ = New-Object System.Windows.Forms.Label
$lblQ.Text = "越小越清晰/越大"
$lblQ.Location = New-Object System.Drawing.Point(585,35)
$lblQ.Size = New-Object System.Drawing.Size(120,25)
$grpVideo.Controls.Add($lblQ)

$chkCrop = New-Object System.Windows.Forms.CheckBox
$chkCrop.Text = "使用裁剪参数"
$chkCrop.Location = New-Object System.Drawing.Point(20,82)
$chkCrop.Size = New-Object System.Drawing.Size(115,25)
$chkCrop.Checked = $true
$grpVideo.Controls.Add($chkCrop)

$txtCrop = New-Object System.Windows.Forms.TextBox
$txtCrop.Location = New-Object System.Drawing.Point(140,80)
$txtCrop.Size = New-Object System.Drawing.Size(200,25)
$txtCrop.Text = ""
$grpVideo.Controls.Add($txtCrop)

$btnCrop = New-Object System.Windows.Forms.Button
$btnCrop.Text = "自动检测黑边"
$btnCrop.Location = New-Object System.Drawing.Point(350,77)
$btnCrop.Size = New-Object System.Drawing.Size(120,32)
$grpVideo.Controls.Add($btnCrop)

$lblCropHelp = New-Object System.Windows.Forms.Label
$lblCropHelp.Text = "格式：crop=宽:高:X:Y"
$lblCropHelp.Location = New-Object System.Drawing.Point(480,83)
$lblCropHelp.Size = New-Object System.Drawing.Size(190,25)
$grpVideo.Controls.Add($lblCropHelp)

$chkYuv420 = New-Object System.Windows.Forms.CheckBox
$chkYuv420.Text = "强制 yuvj420p（老设备兼容）"
$chkYuv420.Location = New-Object System.Drawing.Point(20,118)
$chkYuv420.Size = New-Object System.Drawing.Size(240,25)
$chkYuv420.Checked = $true
$grpVideo.Controls.Add($chkYuv420)

$chkSquare = New-Object System.Windows.Forms.CheckBox
$chkSquare.Text = "方形裁切 460×460"
$chkSquare.Location = New-Object System.Drawing.Point(300,118)
$chkSquare.Size = New-Object System.Drawing.Size(170,25)
$grpVideo.Controls.Add($chkSquare)
$chkSquare.Add_CheckedChanged({
    if ($chkSquare.Checked) {
        # 方形裁切模式推荐参数：20FPS + q=5
        $numFps.Value = 20
        $numQ.Value = 5
    } else {
        # 取消方形裁切，帧率恢复默认 24FPS
        $numFps.Value = 24
    }
    Update-CommandPreview
})

$y += 170

$grpTrim = New-Object System.Windows.Forms.GroupBox
$grpTrim.Text = "片头/片尾裁剪（秒，0 = 不裁剪）"
$grpTrim.Location = New-Object System.Drawing.Point(20,$y)
$grpTrim.Size = New-Object System.Drawing.Size(720,55)
$form.Controls.Add($grpTrim)

$lt1 = New-Object System.Windows.Forms.Label
$lt1.Text = "片头去"
$lt1.Location = New-Object System.Drawing.Point(20,22)
$lt1.Size = New-Object System.Drawing.Size(60,25)
$grpTrim.Controls.Add($lt1)

$numTrimStart = New-Object System.Windows.Forms.NumericUpDown
$numTrimStart.Location = New-Object System.Drawing.Point(80,20)
$numTrimStart.Size = New-Object System.Drawing.Size(80,25)
$numTrimStart.Minimum = 0
$numTrimStart.Maximum = 3600
$numTrimStart.DecimalPlaces = 3
$numTrimStart.Increment = 0.1
$numTrimStart.Value = 0
$grpTrim.Controls.Add($numTrimStart)

$lt2 = New-Object System.Windows.Forms.Label
$lt2.Text = "秒"
$lt2.Location = New-Object System.Drawing.Point(155,22)
$lt2.Size = New-Object System.Drawing.Size(30,25)
$grpTrim.Controls.Add($lt2)

$lt3 = New-Object System.Windows.Forms.Label
$lt3.Text = "片尾去"
$lt3.Location = New-Object System.Drawing.Point(240,22)
$lt3.Size = New-Object System.Drawing.Size(60,25)
$grpTrim.Controls.Add($lt3)

$numTrimEnd = New-Object System.Windows.Forms.NumericUpDown
$numTrimEnd.Location = New-Object System.Drawing.Point(300,20)
$numTrimEnd.Size = New-Object System.Drawing.Size(80,25)
$numTrimEnd.Minimum = 0
$numTrimEnd.Maximum = 3600
$numTrimEnd.DecimalPlaces = 3
$numTrimEnd.Increment = 0.1
$numTrimEnd.Value = 0
$grpTrim.Controls.Add($numTrimEnd)

$lt4 = New-Object System.Windows.Forms.Label
$lt4.Text = "秒"
$lt4.Location = New-Object System.Drawing.Point(375,22)
$lt4.Size = New-Object System.Drawing.Size(30,25)
$grpTrim.Controls.Add($lt4)

$lt5 = New-Object System.Windows.Forms.Label
$lt5.Text = "秒（毫秒精度），点【预览】查看头尾边界帧。"
$lt5.Location = New-Object System.Drawing.Point(388,22)
$lt5.Size = New-Object System.Drawing.Size(170,25)
$lt5.ForeColor = [System.Drawing.Color]::Gray
$grpTrim.Controls.Add($lt5)

$btnTrimPreview = New-Object System.Windows.Forms.Button
$btnTrimPreview.Text = "预览去头尾"
$btnTrimPreview.Location = New-Object System.Drawing.Point(560,16)
$btnTrimPreview.Size = New-Object System.Drawing.Size(140,30)
$grpTrim.Controls.Add($btnTrimPreview)

# 预览窗口的辅助函数。放在脚本作用域，事件脚本块才能调用；
# 窗口状态统一放 $script:ps 哈希表，跨事件脚本块共享。
function preview-show-frame([double]$sec) {
    $ps = $script:ps
    if ($null -eq $ps -or $null -eq $ps.pic) { return }
    $t = $sec.ToString("0.###", $ps.inv)
    & $ps.ffmpeg -hide_banner -y -ss $t -i $ps.input -frames:v 1 -vf "scale=400:400:force_original_aspect_ratio=decrease" -q:v 3 $ps.tmpCur 2>$null | Out-Null
    if (Test-Path -LiteralPath $ps.tmpCur) {
        if ($null -ne $ps.pic.Image) { $ps.pic.Image.Dispose() }
        # 用 MemoryStream 载入：FromFile 会锁住临时文件，导致下一次 ffmpeg -y 覆盖失败，
        # 画面就永远停在第一帧。
        $bytes = [System.IO.File]::ReadAllBytes($ps.tmpCur)
        $ms = New-Object System.IO.MemoryStream(,$bytes)
        $ps.pic.Image = [System.Drawing.Image]::FromStream($ms)
        $ps.pic.Refresh()
    }
}
function preview-update-time {
    $ps = $script:ps
    if ($null -eq $ps -or $null -eq $ps.track) { return }
    $sec = $ps.Dur * $ps.track.Value / 10000.0
    $ps.lblTime.Text = "当前 " + $sec.ToString("0.###", $ps.inv) + " s / 总时长 " + $ps.Dur.ToString("0.###", $ps.inv) + " s"
}
function preview-update-info {
    $ps = $script:ps
    if ($null -eq $ps -or $null -eq $ps.lblInfo) { return }
    $h = $ps.Head.ToString("0.###", $ps.inv)
    $t = $ps.Tail.ToString("0.###", $ps.inv)
    $len = ($ps.Tail - $ps.Head).ToString("0.###", $ps.inv)
    $ps.lblInfo.Text = "片头=$h s   片尾=$t s   剩余=$len s"
}
function preview-on-track-change {
    $ps = $script:ps
    $now = [Environment]::TickCount
    if ($now - $ps.LastExtractMs -ge 150) {
        $ps.LastExtractMs = $now
        preview-show-frame ($ps.Dur * $ps.track.Value / 10000.0)
    }
    preview-update-time
}

$btnTrimPreview.Add_Click({
    if (-not $ffmpeg) {
        [System.Windows.Forms.MessageBox]::Show("找不到 ffmpeg。","缺少 FFmpeg")
        return
    }
    if ([string]::IsNullOrWhiteSpace($txtInput.Text) -or -not (Test-Path -LiteralPath $txtInput.Text)) {
        [System.Windows.Forms.MessageBox]::Show("请先选择有效的视频文件。","提示")
        return
    }
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    $startTrim = [double]$numTrimStart.Value
    $endTrim = [double]$numTrimEnd.Value
    $dur = $script:g_input_duration
    if ($dur -le 0) {
        $dur = Get-DurationSeconds $txtInput.Text $ffmpeg
        $script:g_input_duration = $dur
    }
    if ($dur -le 0) {
        [System.Windows.Forms.MessageBox]::Show("无法读取视频时长，请确认文件可被 ffmpeg 解析。","错误")
        return
    }
    $head = $startTrim
    $tail = $dur - $endTrim
    if ($tail -le $head) {
        [System.Windows.Forms.MessageBox]::Show("裁剪后剩余时长为 0 或负数，请调整片头/片尾秒数。","裁剪参数错误")
        return
    }
    $tmpCur = Join-Path $env:TEMP "fakepod_preview_cur.jpg"
    # 共享状态放脚本作用域，事件脚本块才能读写
    $script:ps = @{
        inv    = $inv
        ffmpeg = $ffmpeg
        input  = $txtInput.Text
        tmpCur = $tmpCur
        Head   = $head
        Tail   = $tail
        Dur    = $dur
        LastExtractMs = 0
        dlg    = $null
        pic    = $null
        lblTime = $null
        lblInfo = $null
        track   = $null
    }

    $dlg = New-Object System.Windows.Forms.Form
    $dlg.Text = "去头尾预览 - 拖动进度条选择边界"
    $dlg.Size = New-Object System.Drawing.Size(620, 650)
    $dlg.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedDialog
    $dlg.MaximizeBox = $false
    $dlg.StartPosition = "CenterParent"
    $script:ps.dlg = $dlg

    $pic = New-Object System.Windows.Forms.PictureBox
    $pic.Location = New-Object System.Drawing.Point(110, 20)
    $pic.Size = New-Object System.Drawing.Size(400, 400)
    $pic.SizeMode = [System.Windows.Forms.PictureBoxSizeMode]::Zoom
    $pic.BackColor = [System.Drawing.Color]::Black
    $dlg.Controls.Add($pic)
    $script:ps.pic = $pic

    $lblTime = New-Object System.Windows.Forms.Label
    $lblTime.Location = New-Object System.Drawing.Point(110, 432)
    $lblTime.Size = New-Object System.Drawing.Size(400, 22)
    $lblTime.TextAlign = [System.Drawing.ContentAlignment]::MiddleCenter
    $dlg.Controls.Add($lblTime)
    $script:ps.lblTime = $lblTime

    $track = New-Object System.Windows.Forms.TrackBar
    $track.Location = New-Object System.Drawing.Point(30, 458)
    $track.Size = New-Object System.Drawing.Size(560, 40)
    $track.Minimum = 0
    $track.Maximum = 10000
    $track.SmallChange = 1
    $track.LargeChange = 100
    $track.TickFrequency = 500
    $dlg.Controls.Add($track)
    $script:ps.track = $track

    $lblInfo = New-Object System.Windows.Forms.Label
    $lblInfo.Location = New-Object System.Drawing.Point(30, 504)
    $lblInfo.Size = New-Object System.Drawing.Size(560, 22)
    $dlg.Controls.Add($lblInfo)
    $script:ps.lblInfo = $lblInfo

    $btnHead = New-Object System.Windows.Forms.Button
    $btnHead.Text = "设为片头"
    $btnHead.Location = New-Object System.Drawing.Point(30, 542)
    $btnHead.Size = New-Object System.Drawing.Size(120, 32)
    $dlg.Controls.Add($btnHead)

    $btnTail = New-Object System.Windows.Forms.Button
    $btnTail.Text = "设为片尾"
    $btnTail.Location = New-Object System.Drawing.Point(160, 542)
    $btnTail.Size = New-Object System.Drawing.Size(120, 32)
    $dlg.Controls.Add($btnTail)

    $btnOK = New-Object System.Windows.Forms.Button
    $btnOK.Text = "确定"
    $btnOK.Location = New-Object System.Drawing.Point(500, 542)
    $btnOK.Size = New-Object System.Drawing.Size(90, 32)
    $dlg.Controls.Add($btnOK)

    preview-update-info
    $script:ps.LastExtractMs = [Environment]::TickCount
    $track.Value = [int][Math]::Round($head / $dur * 10000.0)
    preview-update-time
    preview-show-frame $head

    $track.Add_ValueChanged({ preview-on-track-change })
    $track.Add_Scroll({ preview-show-frame ($script:ps.Dur * $script:ps.track.Value / 10000.0) })
    $btnHead.Add_Click({
        $ps = $script:ps
        $ps.Head = $ps.Dur * $ps.track.Value / 10000.0
        if ($ps.Head -ge $ps.Tail) {
            [System.Windows.Forms.MessageBox]::Show("片头不能晚于片尾，请重新调整。","提示")
            return
        }
        preview-update-info
    })
    $btnTail.Add_Click({
        $ps = $script:ps
        $ps.Tail = $ps.Dur * $ps.track.Value / 10000.0
        if ($ps.Tail -le $ps.Head) {
            [System.Windows.Forms.MessageBox]::Show("片尾不能早于片头，请重新调整。","提示")
            return
        }
        preview-update-info
    })
    $btnOK.Add_Click({
        $ps = $script:ps
        if ($ps.Tail -le $ps.Head) {
            [System.Windows.Forms.MessageBox]::Show("片尾必须晚于片头。","裁剪参数错误")
            return
        }
        $numTrimStart.Value = [decimal][Math]::Min($ps.Head, 3599.999)
        $numTrimEnd.Value = [decimal][Math]::Min($ps.Dur - $ps.Tail, 3599.999)
        $ps.dlg.Close()
    })

    [void]$dlg.ShowDialog()
    if ($pic.Image -ne $null) { $pic.Image.Dispose() }
    Remove-Item -LiteralPath $tmpCur -ErrorAction SilentlyContinue
    $script:ps = $null
})

$y += 70

$grpAudio = New-Object System.Windows.Forms.GroupBox
$grpAudio.Text = "音频参数（MP3）"
$grpAudio.Location = New-Object System.Drawing.Point(20,$y)
$grpAudio.Size = New-Object System.Drawing.Size(720,105)
$form.Controls.Add($grpAudio)

$la1 = New-Object System.Windows.Forms.Label
$la1.Text = "码率"
$la1.Location = New-Object System.Drawing.Point(20,35)
$la1.Size = New-Object System.Drawing.Size(55,25)
$grpAudio.Controls.Add($la1)

$cmbBitrate = New-Object System.Windows.Forms.ComboBox
$cmbBitrate.Location = New-Object System.Drawing.Point(75,32)
$cmbBitrate.Size = New-Object System.Drawing.Size(100,25)
$cmbBitrate.DropDownStyle = "DropDownList"
[void]$cmbBitrate.Items.AddRange(@("96k","128k","160k","192k","256k","320k"))
$cmbBitrate.SelectedItem = "192k"
$grpAudio.Controls.Add($cmbBitrate)

$la2 = New-Object System.Windows.Forms.Label
$la2.Text = "采样率"
$la2.Location = New-Object System.Drawing.Point(215,35)
$la2.Size = New-Object System.Drawing.Size(65,25)
$grpAudio.Controls.Add($la2)

$cmbRate = New-Object System.Windows.Forms.ComboBox
$cmbRate.Location = New-Object System.Drawing.Point(280,32)
$cmbRate.Size = New-Object System.Drawing.Size(105,25)
$cmbRate.DropDownStyle = "DropDownList"
[void]$cmbRate.Items.AddRange(@("保持默认","44100","48000"))
$cmbRate.SelectedIndex = 0
$grpAudio.Controls.Add($cmbRate)

$la3 = New-Object System.Windows.Forms.Label
$la3.Text = "声道"
$la3.Location = New-Object System.Drawing.Point(425,35)
$la3.Size = New-Object System.Drawing.Size(55,25)
$grpAudio.Controls.Add($la3)

$cmbChannels = New-Object System.Windows.Forms.ComboBox
$cmbChannels.Location = New-Object System.Drawing.Point(480,32)
$cmbChannels.Size = New-Object System.Drawing.Size(105,25)
$cmbChannels.DropDownStyle = "DropDownList"
[void]$cmbChannels.Items.AddRange(@("保持默认","单声道","双声道"))
$cmbChannels.SelectedIndex = 0
$grpAudio.Controls.Add($cmbChannels)

$lblAudioNote = New-Object System.Windows.Forms.Label
$lblAudioNote.Text = "输出固定为 AVI + MJPEG + MP3；尺寸保持原画面比例。"
$lblAudioNote.Location = New-Object System.Drawing.Point(20,70)
$lblAudioNote.Size = New-Object System.Drawing.Size(500,25)
$grpAudio.Controls.Add($lblAudioNote)

$y += 120

$lblStatus = New-Object System.Windows.Forms.Label
$lblStatus.Text = "就绪"
$lblStatus.Location = New-Object System.Drawing.Point(20,$y)
$lblStatus.Size = New-Object System.Drawing.Size(720,25)
$form.Controls.Add($lblStatus)
$y += 30

$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(20,$y)
$progress.Size = New-Object System.Drawing.Size(720,20)
$progress.Style = "Blocks"
$form.Controls.Add($progress)
$y += 35

$txtCommand = New-Object System.Windows.Forms.TextBox
$txtCommand.Location = New-Object System.Drawing.Point(20,$y)
$txtCommand.Size = New-Object System.Drawing.Size(720,70)
$txtCommand.Multiline = $true
$txtCommand.ReadOnly = $true
$txtCommand.ScrollBars = "Vertical"
$form.Controls.Add($txtCommand)
$y += 85

$btnConvert = New-Object System.Windows.Forms.Button
$btnConvert.Text = "开始转换"
$btnConvert.Location = New-Object System.Drawing.Point(20,$y)
$btnConvert.Size = New-Object System.Drawing.Size(150,40)
$form.Controls.Add($btnConvert)

$btnOpen = New-Object System.Windows.Forms.Button
$btnOpen.Text = "打开输出目录"
$btnOpen.Location = New-Object System.Drawing.Point(185,$y)
$btnOpen.Size = New-Object System.Drawing.Size(140,40)
$form.Controls.Add($btnOpen)

$btnPreset = New-Object System.Windows.Forms.Button
$btnPreset.Text = "恢复推荐参数"
$btnPreset.Location = New-Object System.Drawing.Point(340,$y)
$btnPreset.Size = New-Object System.Drawing.Size(140,40)
$form.Controls.Add($btnPreset)

$lblTools = New-Object System.Windows.Forms.Label
$lblTools.Location = New-Object System.Drawing.Point(500, ($y + 8))
$lblTools.Size = New-Object System.Drawing.Size(240,28)
$lblTools.TextAlign = "MiddleRight"
$form.Controls.Add($lblTools)

function Update-CommandPreview {
    if ([string]::IsNullOrWhiteSpace($txtInput.Text) -or [string]::IsNullOrWhiteSpace($txtOutput.Text)) {
        $txtCommand.Text = "选择输入视频后，这里会显示实际 FFmpeg 命令。"
        return
    }

    $filters = New-Object System.Collections.Generic.List[string]
    # 黑边裁切（可选）始终在最前，方形裁切叠加在其后。
    if ($chkCrop.Checked -and -not [string]::IsNullOrWhiteSpace($txtCrop.Text)) {
        $cropText = $txtCrop.Text.Trim()
        if ($cropText.StartsWith("crop=")) { $cropText = $cropText.Substring(5) }
        [void]$filters.Add("crop=$cropText")
    }
    if ($chkSquare.Checked) {
        # 方形裁切：短边缩放到边长，长边两侧居中裁掉 → 边长×边长。
        $side = [int]$numWidth.Value
        [void]$filters.Add("scale=${side}:${side}:force_original_aspect_ratio=increase")
        [void]$filters.Add("crop=${side}:${side}")
    } else {
        [void]$filters.Add("scale='if(gt(iw,ih),$([int]$numWidth.Value),-2)':'if(gt(iw,ih),-2,$([int]$numWidth.Value))'")
    }
    [void]$filters.Add("fps=$([int]$numFps.Value)")
    $vf = ($filters -join ",")

    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    $startTrim = [double]$numTrimStart.Value
    $endTrim = [double]$numTrimEnd.Value
    $parts = @("ffmpeg -y")
    if ($startTrim -gt 0) {
        $parts += "-ss " + $startTrim.ToString("0.###", $inv)
    }
    $parts += "-i " + (Quote-Arg $txtInput.Text)
    if ($endTrim -gt 0) {
        $dur = $script:g_input_duration
        $remain = $dur - $startTrim - $endTrim
        if ($dur -gt 0 -and $remain -gt 0) {
            $parts += "-t " + $remain.ToString("0.###", $inv)
        } else {
            $parts += "# 片尾裁剪：请重新选择视频以探测时长"
        }
    }
    $parts += "-vf " + (Quote-Arg $vf)
    $parts += "-c:v mjpeg"
    $parts += "-q:v $([int]$numQ.Value)"
    if ($chkYuv420.Checked) { $parts += "-pix_fmt yuvj420p" }
    $parts += "-c:a libmp3lame"
    $parts += "-b:a $($cmbBitrate.SelectedItem)"
    if ($cmbRate.SelectedItem -ne "保持默认") { $parts += "-ar $($cmbRate.SelectedItem)" }
    if ($cmbChannels.SelectedItem -eq "单声道") { $parts += "-ac 1" }
    if ($cmbChannels.SelectedItem -eq "双声道") { $parts += "-ac 2" }
    $parts += (Quote-Arg $txtOutput.Text)

    $txtCommand.Text = ($parts -join " ")
}

$btnInput.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Filter = "视频文件|*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v|所有文件|*.*"
    if ($dlg.ShowDialog() -eq "OK") {
        $txtInput.Text = $dlg.FileName
        # 探测时长：片尾裁剪和命令预览需要
        if ($ffmpeg) {
            $script:g_input_duration = Get-DurationSeconds $txtInput.Text $ffmpeg
        } else {
            $script:g_input_duration = 0
        }
        $dir = [System.IO.Path]::GetDirectoryName($dlg.FileName)
        $name = [System.IO.Path]::GetFileNameWithoutExtension($dlg.FileName)
        $txtOutput.Text = [System.IO.Path]::Combine($dir, "${name}_460_24fps_MJPEG.avi")
        $txtCrop.Text = ""
        $lblStatus.Text = "已选择视频。需要裁黑边时点击【自动检测黑边】。"
        Update-CommandPreview
    }
})

$btnOutput.Add_Click({
    $dlg = New-Object System.Windows.Forms.SaveFileDialog
    $dlg.Filter = "AVI 视频|*.avi"
    $dlg.DefaultExt = "avi"
    if (-not [string]::IsNullOrWhiteSpace($txtOutput.Text)) {
        $dlg.FileName = [System.IO.Path]::GetFileName($txtOutput.Text)
        $dlg.InitialDirectory = [System.IO.Path]::GetDirectoryName($txtOutput.Text)
    }
    if ($dlg.ShowDialog() -eq "OK") {
        $txtOutput.Text = $dlg.FileName
        Update-CommandPreview
    }
})

$btnCrop.Add_Click({
    if (-not $ffmpeg) {
        [System.Windows.Forms.MessageBox]::Show("找不到 ffmpeg。请先确认 FFmpeg 已安装并能在 PowerShell 中运行。","缺少 FFmpeg")
        return
    }
    if ([string]::IsNullOrWhiteSpace($txtInput.Text) -or -not (Test-Path -LiteralPath $txtInput.Text)) {
        [System.Windows.Forms.MessageBox]::Show("请先选择有效的视频文件。","提示")
        return
    }
    try {
        $btnCrop.Enabled = $false
        $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor
        $crop = Detect-Crop $txtInput.Text $ffmpeg $lblStatus
        if ($crop) {
            $txtCrop.Text = $crop
            $chkCrop.Checked = $true
            $lblStatus.Text = "黑边检测完成：$crop"
        } else {
            $lblStatus.Text = "没有检测到可靠的黑边参数。可以取消【使用裁剪参数】或手动填写。"
        }
    } catch {
        $lblStatus.Text = "黑边检测失败：$($_.Exception.Message)"
    } finally {
        $btnCrop.Enabled = $true
        $form.Cursor = [System.Windows.Forms.Cursors]::Default
        Update-CommandPreview
    }
})

$btnPreset.Add_Click({
    $numWidth.Value = 460
    $numFps.Value = 24
    $numQ.Value = 5
    $cmbBitrate.SelectedItem = "192k"
    $cmbRate.SelectedIndex = 0
    $cmbChannels.SelectedIndex = 0
    $chkYuv420.Checked = $true
    $chkSquare.Checked = $false
    $numTrimStart.Value = 0
    $numTrimEnd.Value = 0
    Update-CommandPreview
})

$btnOpen.Add_Click({
    if (-not [string]::IsNullOrWhiteSpace($txtOutput.Text)) {
        $dir = [System.IO.Path]::GetDirectoryName($txtOutput.Text)
        if (Test-Path -LiteralPath $dir) {
            Start-Process explorer.exe $dir
        }
    }
})

$btnConvert.Add_Click({
    if (-not $ffmpeg) {
        [System.Windows.Forms.MessageBox]::Show("找不到 ffmpeg。请确认 FFmpeg 已安装并且 `ffmpeg -version` 可以运行。","缺少 FFmpeg")
        return
    }
    if ([string]::IsNullOrWhiteSpace($txtInput.Text) -or -not (Test-Path -LiteralPath $txtInput.Text)) {
        [System.Windows.Forms.MessageBox]::Show("输入文件不存在，请重新选择。","错误")
        return
    }
    if ([string]::IsNullOrWhiteSpace($txtOutput.Text)) {
        [System.Windows.Forms.MessageBox]::Show("请选择输出文件。","错误")
        return
    }

    $filters = New-Object System.Collections.Generic.List[string]
    # 黑边裁切（可选）始终在最前，方形裁切叠加在其后。
    if ($chkCrop.Checked -and -not [string]::IsNullOrWhiteSpace($txtCrop.Text)) {
        $cropText = $txtCrop.Text.Trim()
        if ($cropText.StartsWith("crop=")) { $cropText = $cropText.Substring(5) }
        if ($cropText -notmatch '^\d+:\d+:\d+:\d+$') {
            [System.Windows.Forms.MessageBox]::Show("裁剪参数格式不正确。示例：crop=1424:1056:250:8","裁剪参数错误")
            return
        }
        [void]$filters.Add("crop=$cropText")
    }
    if ($chkSquare.Checked) {
        # 方形裁切：短边缩放到边长，长边两侧居中裁掉 → 边长×边长。
        $side = [int]$numWidth.Value
        [void]$filters.Add("scale=${side}:${side}:force_original_aspect_ratio=increase")
        [void]$filters.Add("crop=${side}:${side}")
    } else {
        [void]$filters.Add("scale='if(gt(iw,ih),$([int]$numWidth.Value),-2)':'if(gt(iw,ih),-2,$([int]$numWidth.Value))'")
    }
    [void]$filters.Add("fps=$([int]$numFps.Value)")
    $vf = ($filters -join ",")

    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    $startTrim = [double]$numTrimStart.Value
    $endTrim = [double]$numTrimEnd.Value
    $argList = New-Object System.Collections.Generic.List[string]
    [void]$argList.Add("-y")
    if ($startTrim -gt 0) {
        [void]$argList.Add("-ss")
        [void]$argList.Add($startTrim.ToString("0.###", $inv))
    }
    [void]$argList.Add("-i")
    [void]$argList.Add((Quote-Arg $txtInput.Text))
    if ($endTrim -gt 0) {
        $dur = Get-DurationSeconds $txtInput.Text $ffmpeg
        $script:g_input_duration = $dur
        $remain = $dur - $startTrim - $endTrim
        if ($remain -le 0) {
            [System.Windows.Forms.MessageBox]::Show("裁剪后剩余时长为 0 或负数，请调整片头/片尾秒数。","裁剪参数错误")
            return
        }
        [void]$argList.Add("-t")
        [void]$argList.Add($remain.ToString("0.###", $inv))
    }
    [void]$argList.Add("-vf")
    [void]$argList.Add((Quote-Arg $vf))
    [void]$argList.Add("-c:v")
    [void]$argList.Add("mjpeg")
    [void]$argList.Add("-q:v")
    [void]$argList.Add("$([int]$numQ.Value)")
    if ($chkYuv420.Checked) {
        [void]$argList.Add("-pix_fmt")
        [void]$argList.Add("yuvj420p")
    }
    [void]$argList.Add("-c:a")
    [void]$argList.Add("libmp3lame")
    [void]$argList.Add("-b:a")
    [void]$argList.Add("$($cmbBitrate.SelectedItem)")
    if ($cmbRate.SelectedItem -ne "保持默认") {
        [void]$argList.Add("-ar")
        [void]$argList.Add("$($cmbRate.SelectedItem)")
    }
    if ($cmbChannels.SelectedItem -eq "单声道") {
        [void]$argList.Add("-ac"); [void]$argList.Add("1")
    } elseif ($cmbChannels.SelectedItem -eq "双声道") {
        [void]$argList.Add("-ac"); [void]$argList.Add("2")
    }
    [void]$argList.Add((Quote-Arg $txtOutput.Text))

    $arguments = ($argList -join " ")
    Update-CommandPreview

    $outDir = [System.IO.Path]::GetDirectoryName($txtOutput.Text)
    if (-not (Test-Path -LiteralPath $outDir)) {
        [System.IO.Directory]::CreateDirectory($outDir) | Out-Null
    }

    $btnConvert.Enabled = $false
    $btnCrop.Enabled = $false
    $progress.Style = "Marquee"
    $progress.MarqueeAnimationSpeed = 30
    $lblStatus.Text = "正在转换，请勿关闭本窗口…"
    $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor
    [System.Windows.Forms.Application]::DoEvents()

    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ffmpeg
        $psi.Arguments = $arguments
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true

        $p = New-Object System.Diagnostics.Process
        $p.StartInfo = $psi
        [void]$p.Start()

        while (-not $p.HasExited) {
            [System.Windows.Forms.Application]::DoEvents()
            Start-Sleep -Milliseconds 100
        }

        if ($p.ExitCode -eq 0 -and (Test-Path -LiteralPath $txtOutput.Text)) {
            $lblStatus.Text = "转换完成：$($txtOutput.Text)"
            [System.Media.SystemSounds]::Asterisk.Play()
            [System.Windows.Forms.MessageBox]::Show("转换完成。`r`n`r`n$($txtOutput.Text)","完成")
        } else {
            $lblStatus.Text = "转换失败，FFmpeg 退出代码：$($p.ExitCode)"
            [System.Windows.Forms.MessageBox]::Show("转换失败。FFmpeg 退出代码：$($p.ExitCode)`r`n可复制界面中的命令到 PowerShell 查看详细错误。","失败")
        }
    } catch {
        $lblStatus.Text = "转换失败：$($_.Exception.Message)"
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message,"转换失败")
    } finally {
        $progress.Style = "Blocks"
        $progress.Value = 0
        $btnConvert.Enabled = $true
        $btnCrop.Enabled = $true
        $form.Cursor = [System.Windows.Forms.Cursors]::Default
    }
})

foreach ($c in @($txtInput,$txtOutput,$numWidth,$numFps,$numQ,$chkCrop,$txtCrop,$chkYuv420,$cmbBitrate,$cmbRate,$cmbChannels,$numTrimStart,$numTrimEnd)) {
    if ($c -is [System.Windows.Forms.TextBox]) {
        $c.Add_TextChanged({ Update-CommandPreview })
    } elseif ($c -is [System.Windows.Forms.NumericUpDown]) {
        $c.Add_ValueChanged({ Update-CommandPreview })
    } elseif ($c -is [System.Windows.Forms.CheckBox]) {
        $c.Add_CheckedChanged({ Update-CommandPreview })
    } elseif ($c -is [System.Windows.Forms.ComboBox]) {
        $c.Add_SelectedIndexChanged({ Update-CommandPreview })
    }
}

if ($ffmpeg) {
    $lblTools.Text = "FFmpeg：已检测"
    $lblStatus.Text = "FFmpeg 已就绪。"
} else {
    $lblTools.Text = "FFmpeg：未检测到"
    $lblStatus.Text = "未检测到 FFmpeg。请先确保 ffmpeg -version 可运行。"
}

Update-CommandPreview
[void]$form.ShowDialog()
