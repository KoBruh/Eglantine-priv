// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.StatFs
import android.text.format.Formatter
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import androidx.navigation.fragment.findNavController
import androidx.navigation.fragment.navArgs
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.transition.MaterialSharedAxis
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.FragmentGameInfoBinding
import org.yuzu.yuzu_emu.model.EdenPackResult
import org.yuzu.yuzu_emu.model.GameVerificationResult
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.GameMetadata
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins

class GameInfoFragment : Fragment() {
    private var _binding: FragmentGameInfoBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val gamesViewModel: GamesViewModel by activityViewModels()

    private val args by navArgs<GameInfoFragmentArgs>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)

        // Check for an up-to-date version string
        args.game.version = GameMetadata.getVersion(args.game.path, true)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentGameInfoBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(false)

        binding.apply {
            toolbarInfo.title = args.game.title
            toolbarInfo.setNavigationOnClickListener {
                requireActivity().onBackPressedDispatcher.onBackPressed()
            }

            val pathString = Uri.parse(args.game.path).path ?: ""
            path.setHint(R.string.path)
            pathField.setText(pathString)
            pathField.setOnClickListener { copyToClipboard(getString(R.string.path), pathString) }

            programId.setHint(R.string.program_id)
            programIdField.setText(args.game.programIdHex)
            programIdField.setOnClickListener {
                copyToClipboard(getString(R.string.program_id), args.game.programIdHex)
            }

            if (args.game.developer.isNotEmpty()) {
                developer.setHint(R.string.developer)
                developerField.setText(args.game.developer)
                developerField.setOnClickListener {
                    copyToClipboard(getString(R.string.developer), args.game.developer)
                }
            } else {
                developer.setVisible(false)
            }

            version.setHint(R.string.version)
            versionField.setText(args.game.version)
            versionField.setOnClickListener {
                copyToClipboard(getString(R.string.version), args.game.version)
            }

            buttonCopy.setOnClickListener {
                val details = """
                    ${args.game.title}
                    ${getString(R.string.path)} - $pathString
                    ${getString(R.string.program_id)} - ${args.game.programIdHex}
                    ${getString(R.string.developer)} - ${args.game.developer}
                    ${getString(R.string.version)} - ${args.game.version}
                """.trimIndent()
                copyToClipboard(args.game.title, details)
            }

            buttonVerifyIntegrity.setOnClickListener {
                ProgressDialogFragment.newInstance(
                    requireActivity(),
                    R.string.verifying,
                    true
                ) { progressCallback, _ ->
                    val result = GameVerificationResult.from(
                        NativeLibrary.verifyGameContents(
                            args.game.path,
                            progressCallback
                        )
                    )
                    return@newInstance when (result) {
                        GameVerificationResult.Success ->
                            MessageDialogFragment.newInstance(
                                titleId = R.string.verify_success,
                                descriptionId = R.string.operation_completed_successfully
                            )

                        GameVerificationResult.Failed ->
                            MessageDialogFragment.newInstance(
                                titleId = R.string.verify_failure,
                                descriptionId = R.string.verify_failure_description
                            )

                        GameVerificationResult.NotImplemented ->
                            MessageDialogFragment.newInstance(
                                titleId = R.string.verify_no_result,
                                descriptionId = R.string.verify_no_result_description
                            )
                    }
                }.show(parentFragmentManager, ProgressDialogFragment.TAG)
            }

            val dumpRoot = NativeLibrary.edenPackDumpRoot(args.game.path)
            buttonConvertEdenpack.setVisible(dumpRoot != null)
            buttonConvertEdenpack.setOnClickListener {
                dumpRoot?.let { confirmEdenPackConversion(it) }
            }
        }

        setInsets()
    }

    private fun confirmEdenPackConversion(root: String) {
        val target = NativeLibrary.edenPackTargetPath(root)
        if (File(target).exists()) {
            MessageDialogFragment.newInstance(
                requireActivity(),
                titleId = R.string.edenpack_failed,
                descriptionString = getString(R.string.edenpack_exists, target)
            ).show(parentFragmentManager, MessageDialogFragment.TAG)
            return
        }
        if (NativeLibrary.edenPackHasJournal(target)) {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.edenpack_resume_title)
                .setMessage(getString(R.string.edenpack_resume_description, target))
                .setPositiveButton(R.string.edenpack_resume) { _, _ ->
                    convertToEdenPack(root, target, true)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
            return
        }

        // The pack holds the chosen exefs and romfs, which sit together under the update or base.
        val contentRoot = File(args.game.path).parentFile?.parentFile ?: return
        viewLifecycleOwner.lifecycleScope.launch {
            val needed = withContext(Dispatchers.IO) {
                contentRoot.walkTopDown().filter { it.isFile }.sumOf { it.length() }
            }
            val free = StatFs(File(target).parent).availableBytes
            val context = context ?: return@launch
            var message = getString(
                R.string.edenpack_convert_description,
                target,
                Formatter.formatFileSize(context, needed),
                Formatter.formatFileSize(context, free)
            )
            if (free < needed) {
                message += "\n\n" + getString(R.string.edenpack_not_enough_space)
            }
            val lowStorage = CheckBox(context).apply {
                setText(R.string.edenpack_low_storage)
                isChecked = free < needed
            }
            MaterialAlertDialogBuilder(context)
                .setTitle(R.string.edenpack_convert)
                .setMessage(message)
                .setView(lowStorage)
                .setPositiveButton(R.string.edenpack_convert) { _, _ ->
                    convertToEdenPack(root, target, lowStorage.isChecked)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun convertToEdenPack(root: String, target: String, deleteSources: Boolean) {
        val context = requireContext().applicationContext
        val mib = 1024L * 1024L
        val dialog = ProgressDialogFragment.newInstance(
            requireActivity(),
            R.string.edenpack_converting,
            true
        ) { progressCallback, messageCallback ->
            val result = EdenPackResult.from(
                NativeLibrary.convertToEdenPack(root, target, deleteSources) {
                        bytesTotal, bytesDone, filesTotal, filesDone ->
                    messageCallback(
                        context.getString(
                            R.string.edenpack_progress,
                            bytesDone / mib,
                            bytesTotal / mib,
                            filesDone,
                            filesTotal
                        )
                    )
                    progressCallback(bytesTotal, bytesDone)
                }
            )
            when (result) {
                EdenPackResult.Success -> MessageDialogFragment.newInstance(
                    titleId = R.string.edenpack_done,
                    descriptionString = context.getString(
                        if (deleteSources) {
                            R.string.edenpack_done_low_storage_description
                        } else {
                            R.string.edenpack_done_description
                        },
                        target
                    )
                )

                EdenPackResult.Cancelled -> if (deleteSources) {
                    MessageDialogFragment.newInstance(
                        titleId = R.string.edenpack_convert,
                        descriptionId = R.string.edenpack_cancelled_resumable
                    )
                } else {
                    Unit
                }

                else -> MessageDialogFragment.newInstance(
                    titleId = R.string.edenpack_failed,
                    descriptionString = NativeLibrary.edenPackLastError()
                )
            }
        }
        dialog.onDialogComplete = {
            gamesViewModel.reloadGames(false)
            if (File(target).exists() && isAdded) {
                findNavController().popBackStack()
            }
        }
        dialog.show(parentFragmentManager, ProgressDialogFragment.TAG)
    }

    private fun copyToClipboard(label: String, body: String) {
        val clipBoard =
            requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = ClipData.newPlainText(label, body)
        clipBoard.setPrimaryClip(clip)

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(
                requireContext(),
                R.string.copied_to_clipboard,
                Toast.LENGTH_SHORT
            ).show()
        }
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { _: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right

            binding.toolbarInfo.updateMargins(left = leftInsets, right = rightInsets)
            binding.scrollInfo.updateMargins(left = leftInsets, right = rightInsets)

            binding.contentInfo.updatePadding(bottom = barInsets.bottom)

            windowInsets
        }
}
